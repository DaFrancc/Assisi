#!/usr/bin/env python3
"""reflectgen.py — Assisi Reflection Code Generator

Scans C++ headers for ACOMP/AFIELD annotations and emits .generated.cpp files
that register each component with Assisi::Core::Reflect::ComponentRegistry.

Usage:
    python reflectgen.py <header> [<header> ...] --outdir <dir> [--include <path>]

    <header>         Absolute or relative path to the source header.
    --outdir <dir>   Directory to write .generated.cpp files into.
    --include <path> Override the #include path written into generated files.
                     If omitted, auto-detected from the 'include/' segment in
                     the header path (e.g. '.../include/Assisi/Foo/Bar.hpp'
                     becomes 'Assisi/Foo/Bar.hpp').
"""

import re
import sys
import argparse
from pathlib import Path
from dataclasses import dataclass, field
from typing import Optional

# ──────────────────────────────────────────────────────────────────────────────
# Data model
# ──────────────────────────────────────────────────────────────────────────────

@dataclass
class AnnotArgs:
    flags: set = field(default_factory=set)
    kvs:   dict = field(default_factory=dict)

    def has(self, flag: str) -> bool:
        return flag in self.flags

    def get(self, key: str, default=None):
        return self.kvs.get(key, default)


@dataclass
class EnumInfo:
    name:      str                 # unqualified, e.g. 'ColliderShape'
    fqn:       str                 # fully-qualified, e.g. 'Assisi::Physics::ColliderShape'
    constants: list                # list[tuple[str, int]] in declaration order


@dataclass
class RadioInfo:
    """Resolved AFIELD(radio ...) metadata for one field.

    A *broadcaster* (AFIELD(radioBroadcast) on an enum) sets is_broadcast. A
    *listener* (AFIELD(radioListen = { source, value, behavior })) sets
    source/values/behavior, with `values` already resolved from enumerator names
    to their integer values. A field can be BOTH — an enum that follows another
    broadcaster while broadcasting to its own listeners (a radio chain).
    """
    is_broadcast: bool = False     # AFIELD(radioBroadcast) flag on an enum field
    source:       str  = ''        # sibling broadcaster enum field name ('' = not a listener)
    values:       list = field(default_factory=list)  # list[int]: active enum values
    behavior:     str  = 'None'    # 'None' | 'Grey' | 'Vanish'


@dataclass
class FieldInfo:
    name:      str
    cpp_type:  str
    args:      AnnotArgs
    enum_info: Optional[EnumInfo]  = None  # set when cpp_type names an AENUM enum
    radio:     Optional[RadioInfo] = None  # set by _resolve_radio after parsing


@dataclass
class ComponentInfo:
    name:       str
    namespaces: list   # e.g. ['Assisi', 'Runtime']
    args:       AnnotArgs
    fields:     list   # list[FieldInfo]
    is_asset:   bool = False  # True for AASSET (standalone asset), False for ACOMP


# ──────────────────────────────────────────────────────────────────────────────
# Type → codegen mapping
# ──────────────────────────────────────────────────────────────────────────────

@dataclass
class TypeCodegen:
    enum_value:  str  # Core::Reflect::FieldType::Xxx
    serialize:   str  # expression; {a} = member accessor e.g. "c.foo"
    deserialize: str  # statement;  {f} = field name, {a} = accessor


# Shared codegen for std::vector<Core::AssetPath> — an IIFE builds the JSON
# array on serialize; deserialize clears then repopulates. Defined once because
# several element-name spellings (unqualified/qualified) map to it.
_ASSET_PATH_VECTOR = TypeCodegen(
    'AssetPathVector',
    '[&]{{ nlohmann::json _arr = nlohmann::json::array(); '
    'for (const auto& _p : {a}) _arr.push_back(std::string(_p.View())); return _arr; }}()',
    '{{ if (j.contains("{f}")) {{ {a}.clear(); '
    'for (const auto& _e : j.at("{f}")) {{ Assisi::Core::AssetPath _p; '
    '_p.Assign(_e.get<std::string>()); {a}.push_back(_p); }} }} }}')


# Shared codegen for std::vector<Core::AssetId> — the material-override list.
# Each element is a { guid, path-hint } object built by SerializeAssetId; the
# hint is discarded on load. Mirrors _ASSET_PATH_VECTOR but routes through the
# AssetId JSON helpers instead of raw strings.
_ASSET_ID_VECTOR = TypeCodegen(
    'AssetIdVector',
    '[&]{{ nlohmann::json _arr = nlohmann::json::array(); '
    'for (const auto& _e : {a}) _arr.push_back(Assisi::Core::SerializeAssetId(_e)); return _arr; }}()',
    '{{ if (j.contains("{f}")) {{ {a}.clear(); '
    'for (const auto& _e : j.at("{f}")) {a}.push_back(Assisi::Core::DeserializeAssetId(_e)); }} }}')


# Serialize expressions produce values for json initializer lists.
# Deserialize statements read from j.at("{f}") and assign to comp.{f}.
#
# GLM quat: memory layout {x,y,z,w}, constructor glm::quat(w,x,y,z).
# We serialize as [w,x,y,z] to match math convention.
# The deserialize template's {a} is the write target (accessor). It is
# "comp.<field>" for a component and "a.<field>" for an AASSET instance, so
# every template must route writes through {a} — never a hardcoded "comp.".
TYPES: dict[str, TypeCodegen] = {
    'float':     TypeCodegen(
        'Float',
        '{a}',
        'if (j.contains("{f}")) {a} = j.at("{f}").get<float>();'),
    'double':    TypeCodegen(
        'Double',
        '{a}',
        'if (j.contains("{f}")) {a} = j.at("{f}").get<double>();'),
    'int':       TypeCodegen(
        'Int',
        '{a}',
        'if (j.contains("{f}")) {a} = j.at("{f}").get<int>();'),
    'int32_t':   TypeCodegen(
        'Int32',
        '{a}',
        'if (j.contains("{f}")) {a} = j.at("{f}").get<int32_t>();'),
    'uint32_t':  TypeCodegen(
        'UInt32',
        '{a}',
        'if (j.contains("{f}")) {a} = j.at("{f}").get<uint32_t>();'),
    'bool':      TypeCodegen(
        'Bool',
        '{a}',
        'if (j.contains("{f}")) {a} = j.at("{f}").get<bool>();'),
    'glm::vec2': TypeCodegen(
        'Vec2',
        '{{ {a}.x, {a}.y }}',
        '{{ if (j.contains("{f}")) {{ const auto& _v = j.at("{f}"); {a} = {{ _v[0].get<float>(), _v[1].get<float>() }}; }} }}'),
    'glm::vec3': TypeCodegen(
        'Vec3',
        '{{ {a}.x, {a}.y, {a}.z }}',
        '{{ if (j.contains("{f}")) {{ const auto& _v = j.at("{f}"); {a} = {{ _v[0].get<float>(), _v[1].get<float>(), _v[2].get<float>() }}; }} }}'),
    'glm::vec4': TypeCodegen(
        'Vec4',
        '{{ {a}.x, {a}.y, {a}.z, {a}.w }}',
        '{{ if (j.contains("{f}")) {{ const auto& _v = j.at("{f}"); {a} = {{ _v[0].get<float>(), _v[1].get<float>(), _v[2].get<float>(), _v[3].get<float>() }}; }} }}'),
    'glm::quat': TypeCodegen(
        'Quat',
        '{{ {a}.w, {a}.x, {a}.y, {a}.z }}',
        '{{ if (j.contains("{f}")) {{ const auto& _v = j.at("{f}"); {a} = glm::quat{{ _v[0].get<float>(), _v[1].get<float>(), _v[2].get<float>(), _v[3].get<float>() }}; }} }}'),
    # glm::mat4 — 16 floats in column-major order (m[col][row]). Serialized as a
    # flat JSON array of 16; glm::mat4's 16-scalar constructor consumes the same
    # column-major order, so the round-trip is exact.
    'glm::mat4': TypeCodegen(
        'Mat4',
        '{{ {a}[0][0], {a}[0][1], {a}[0][2], {a}[0][3], {a}[1][0], {a}[1][1], {a}[1][2], {a}[1][3], {a}[2][0], {a}[2][1], {a}[2][2], {a}[2][3], {a}[3][0], {a}[3][1], {a}[3][2], {a}[3][3] }}',
        '{{ if (j.contains("{f}")) {{ const auto& _v = j.at("{f}"); {a} = glm::mat4{{ _v[0].get<float>(), _v[1].get<float>(), _v[2].get<float>(), _v[3].get<float>(), _v[4].get<float>(), _v[5].get<float>(), _v[6].get<float>(), _v[7].get<float>(), _v[8].get<float>(), _v[9].get<float>(), _v[10].get<float>(), _v[11].get<float>(), _v[12].get<float>(), _v[13].get<float>(), _v[14].get<float>(), _v[15].get<float>() }}; }} }}'),
    # ECS::Entity — serialized as a stable serial index via SceneSerializer.
    # Accepts both qualified and unqualified names.
    'ECS::Entity': TypeCodegen(
        'EntityRef',
        '({a} != Assisi::ECS::NullEntity ? nlohmann::json(Assisi::Runtime::SceneSerializer::EntityToIndex({a}).value_or(~0u)) : nlohmann::json(nullptr))',
        '{{ if (j.contains("{f}") && !j.at("{f}").is_null()) {{ {a} = Assisi::Runtime::SceneSerializer::IndexToEntity(j.at("{f}").get<uint32_t>()); }} else {{ {a} = Assisi::ECS::NullEntity; }} }}'),
    'Assisi::ECS::Entity': TypeCodegen(
        'EntityRef',
        '({a} != Assisi::ECS::NullEntity ? nlohmann::json(Assisi::Runtime::SceneSerializer::EntityToIndex({a}).value_or(~0u)) : nlohmann::json(nullptr))',
        '{{ if (j.contains("{f}") && !j.at("{f}").is_null()) {{ {a} = Assisi::Runtime::SceneSerializer::IndexToEntity(j.at("{f}").get<uint32_t>()); }} else {{ {a} = Assisi::ECS::NullEntity; }} }}'),
    # Core::AssetPath — a fixed-capacity virtual asset path. Serialized as a JSON
    # string of its view; Assign() re-imposes the length limit on load. Accepts
    # both qualified and unqualified names.
    'AssetPath': TypeCodegen(
        'AssetPath',
        'std::string({a}.View())',
        '{{ if (j.contains("{f}")) {a}.Assign(j.at("{f}").get<std::string>()); }}'),
    'Assisi::Core::AssetPath': TypeCodegen(
        'AssetPath',
        'std::string({a}.View())',
        '{{ if (j.contains("{f}")) {a}.Assign(j.at("{f}").get<std::string>()); }}'),
    # std::vector<Core::AssetPath> — a variable-length list of virtual paths
    # (e.g. MeshRenderer's per-slot material overrides). Serialized as a JSON
    # array of strings; deserialize clears then rebuilds, so a shorter saved
    # array shrinks the vector rather than leaving stale tail entries. Accepts
    # unqualified and qualified element spellings (no internal-whitespace forms;
    # keep the declaration spelled like the keys below).
    'std::vector<AssetPath>':               _ASSET_PATH_VECTOR,
    'std::vector<Core::AssetPath>':         _ASSET_PATH_VECTOR,
    'std::vector<Assisi::Core::AssetPath>': _ASSET_PATH_VECTOR,
    # Core::AssetId — a stable GUID reference. Serialized as { guid, path-hint }
    # via the Core AssetId JSON helpers; deserialize reads the guid and discards
    # the hint (see AssetIdJson.hpp / decision D2). Accepts every spelling.
    'AssetId': TypeCodegen(
        'AssetId',
        'Assisi::Core::SerializeAssetId({a})',
        '{{ if (j.contains("{f}")) {a} = Assisi::Core::DeserializeAssetId(j.at("{f}")); }}'),
    'Core::AssetId': TypeCodegen(
        'AssetId',
        'Assisi::Core::SerializeAssetId({a})',
        '{{ if (j.contains("{f}")) {a} = Assisi::Core::DeserializeAssetId(j.at("{f}")); }}'),
    'Assisi::Core::AssetId': TypeCodegen(
        'AssetId',
        'Assisi::Core::SerializeAssetId({a})',
        '{{ if (j.contains("{f}")) {a} = Assisi::Core::DeserializeAssetId(j.at("{f}")); }}'),
    # std::vector<Core::AssetId> — MeshRenderer's per-slot material overrides.
    'std::vector<AssetId>':               _ASSET_ID_VECTOR,
    'std::vector<Core::AssetId>':         _ASSET_ID_VECTOR,
    'std::vector<Assisi::Core::AssetId>': _ASSET_ID_VECTOR,
}

# Field types whose codegen calls the Core AssetId JSON helpers; a generated file
# with any such field must include the helper header.
_ASSET_ID_TYPES = {
    'AssetId', 'Core::AssetId', 'Assisi::Core::AssetId',
    'std::vector<AssetId>', 'std::vector<Core::AssetId>', 'std::vector<Assisi::Core::AssetId>',
}

# reflectgen is default-deny: any non-transient AFIELD whose type is not in
# TYPES is a hard generation error (see _check_unsupported), because emitting a
# stub that silently drops that field on every save is worse than failing the
# build. This dict is optional colour on that failure: a known-unsupported type
# mapped to a human reason ("no string codegen yet") produces a better message
# than the generic default. Fields listed here fail exactly like any other
# unknown type — the entry only improves the diagnostic.
UNSUPPORTED_TYPES: dict[str, str] = {}


# ──────────────────────────────────────────────────────────────────────────────
# Annotation argument parser
# ──────────────────────────────────────────────────────────────────────────────

def _split_args(s: str) -> list[str]:
    """Split by comma, respecting quoted strings and {brace} nesting.

    Brace-awareness is what lets a single AFIELD argument carry a nested object
    with its own commas, e.g. `radio = { listen = shape, value = {A, B} }` — only
    top-level commas separate arguments. The same helper splits that nested body.
    """
    result, current, in_quote, qchar, depth = [], [], False, '', 0
    for ch in s:
        if in_quote:
            current.append(ch)
            if ch == qchar:
                in_quote = False
        elif ch in ('"', "'"):
            in_quote, qchar = True, ch
            current.append(ch)
        elif ch == '{':
            depth += 1
            current.append(ch)
        elif ch == '}':
            depth -= 1
            current.append(ch)
        elif ch == ',' and depth == 0:
            result.append(''.join(current))
            current = []
        else:
            current.append(ch)
    if current:
        result.append(''.join(current))
    return result


def parse_annot_args(content: str) -> AnnotArgs:
    """Parse the argument list inside ACOMP(...) or AFIELD(...)."""
    args = AnnotArgs()
    for token in _split_args(content):
        token = token.strip()
        if not token:
            continue
        if '=' in token:
            k, _, v = token.partition('=')
            args.kvs[k.strip()] = v.strip().strip("\"'")
        else:
            args.flags.add(token)
    return args


# ──────────────────────────────────────────────────────────────────────────────
# Comment stripping
# ──────────────────────────────────────────────────────────────────────────────

def strip_comments(text: str) -> str:
    """Remove C and C++ comments while preserving line structure."""
    result, i, n = [], 0, len(text)
    while i < n:
        if text[i:i+2] == '//':
            while i < n and text[i] != '\n':
                result.append(' ')
                i += 1
        elif text[i:i+2] == '/*':
            result.append(' ')
            result.append(' ')
            i += 2
            while i < n and text[i:i+2] != '*/':
                result.append('\n' if text[i] == '\n' else ' ')
                i += 1
            if i < n:
                result.append(' ')
                result.append(' ')
                i += 2
        else:
            result.append(text[i])
            i += 1
    return ''.join(result)


# ──────────────────────────────────────────────────────────────────────────────
# Header parser
# ──────────────────────────────────────────────────────────────────────────────

_ACOMP_RE  = re.compile(r'\bACOMP\s*\(([^)]*)\)')
_AASSET_RE = re.compile(r'\bAASSET\s*\(([^)]*)\)')
_AENUM_RE  = re.compile(r'\bAENUM\s*\(([^)]*)\)')
_AFIELD_RE = re.compile(r'\bAFIELD\s*\(([^)]*)\)')
_STRUCT_RE = re.compile(r'\bstruct\s+(\w+)')
# `enum class Name` / `enum struct Name`, with an optional `: underlying` — the
# name is captured; the brace body is extracted separately.
_ENUM_RE   = re.compile(r'\benum\s+(?:class|struct)\s+(\w+)\s*(?::\s*[\w:]+\s*)?')
_NS_RE     = re.compile(r'\bnamespace\s+([\w:]+)')


def parse_enum_constants(body: str) -> list:
    """Parse an enum body into [(name, value), ...] in declaration order.

    Handles implicit auto-increment and explicit integer values (decimal or
    0x-hex, possibly negative). A non-integer initializer (e.g. referencing
    another constant or an expression) is a hard error — reflectgen needs the
    concrete value to serialize by number and to drive the editor combo.
    """
    constants: list = []
    next_value = 0
    for raw in body.split(','):
        entry = raw.strip()
        if not entry:
            continue
        if '=' in entry:
            enum_name, _, raw_value = entry.partition('=')
            enum_name = enum_name.strip()
            try:
                value = int(raw_value.strip(), 0)
            except ValueError:
                raise ValueError(f"AENUM enumerator '{enum_name}' has a non-integer "
                                 f"value '{raw_value.strip()}'; only integer literals are supported")
        else:
            enum_name = entry
            value = next_value
        constants.append((enum_name, value))
        next_value = value + 1
    return constants


# ──────────────────────────────────────────────────────────────────────────────
# Radio (declarative editor visibility) parsing + validation
# ──────────────────────────────────────────────────────────────────────────────

# behavior spelling (lower-case in the annotation) → FieldMeta enumerator.
_RADIO_BEHAVIORS = {'grey': 'Grey', 'vanish': 'Vanish'}
_RADIO_KEYS      = {'source', 'value', 'behavior'}


def _parse_value_list(raw: str) -> list[str]:
    """Parse a radio `value` — a single enumerator `E1` or a set `{E1, E2}` —
    into a list of enumerator-name strings."""
    raw = raw.strip()
    if raw.startswith('{') and raw.endswith('}'):
        return [tok.strip() for tok in _split_args(raw[1:-1]) if tok.strip()]
    return [raw] if raw else []


def parse_radio_spec(raw: str, where: str) -> dict:
    """Parse the `{ source = ..., value = ..., behavior = ... }` object of an
    AFIELD(radioListen = {...}) listener into a {key: raw_value} dict. `where`
    names the field for diagnostics. Structure errors are hard failures."""
    raw = raw.strip()
    if not (raw.startswith('{') and raw.endswith('}')):
        raise ValueError(
            f"{where}: AFIELD(radioListen = ...) must be a brace object "
            f"'{{ source = ..., value = ..., behavior = ... }}' (got: '{raw}').")
    spec: dict = {}
    for tok in _split_args(raw[1:-1]):
        tok = tok.strip()
        if not tok:
            continue
        if '=' not in tok:
            raise ValueError(
                f"{where}: AFIELD(radio) sub-argument '{tok}' is not a key = value pair.")
        key, _, value = tok.partition('=')
        spec[key.strip()] = value.strip()
    return spec


def _resolve_radio(comp: 'ComponentInfo', header_name: str) -> None:
    """Resolve and validate every field's AFIELD(radio ...) against its struct,
    attaching a RadioInfo to each field. A field may be a broadcaster
    (AFIELD(radioBroadcast) on an enum), a listener (AFIELD(radioListen = {
    source, value, behavior })), or both — listeners can follow a broadcaster
    that itself follows another, forming a chain.

    Every misuse is a hard build failure: a broadcaster that isn't an enum, a
    listener naming a missing / non-enum / non-broadcaster field, an unknown
    enumerator, a bad behavior, or a cycle in the source chain."""
    by_name = {f.name: f for f in comp.fields}

    for f in comp.fields:
        where         = f"{header_name}: field '{comp.name}::{f.name}'"
        has_broadcast = f.args.has('radioBroadcast')   # AFIELD(radioBroadcast)
        raw_spec      = f.args.get('radioListen')      # AFIELD(radioListen={..})
        info          = RadioInfo()

        if has_broadcast:
            if f.enum_info is None:
                raise ValueError(
                    f"{where} is marked AFIELD(radioBroadcast) but is not an AENUM enum "
                    f"field (its type is '{f.cpp_type}'). Only enum fields can broadcast.")
            info.is_broadcast = True

        if raw_spec is not None:
            spec = parse_radio_spec(raw_spec, where)
            missing = _RADIO_KEYS - spec.keys()
            unknown = spec.keys() - _RADIO_KEYS
            if missing:
                raise ValueError(
                    f"{where}: AFIELD(radioListen) is missing {sorted(missing)}; it needs "
                    f"source, value, and behavior.")
            if unknown:
                raise ValueError(
                    f"{where}: AFIELD(radioListen) has unknown key(s) {sorted(unknown)}; only "
                    f"source, value, behavior are allowed.")

            source_name = spec['source']
            if source_name == f.name:
                raise ValueError(f"{where}: AFIELD(radioListen) cannot listen to itself.")
            source = by_name.get(source_name)
            if source is None:
                raise ValueError(
                    f"{where}: AFIELD(radioListen source = {source_name}) names no field in "
                    f"struct '{comp.name}'.")
            if source.enum_info is None:
                raise ValueError(
                    f"{where}: AFIELD(radioListen) follows '{source_name}', which is not an "
                    f"AENUM enum field.")
            if not source.args.has('radioBroadcast'):
                raise ValueError(
                    f"{where}: AFIELD(radioListen) follows '{source_name}', which is not "
                    f"marked AFIELD(radioBroadcast).")

            value_names = _parse_value_list(spec['value'])
            if not value_names:
                raise ValueError(f"{where}: AFIELD(radioListen value = ...) is empty.")
            const_map = {name: val for name, val in source.enum_info.constants}
            values: list = []
            for vname in value_names:
                if vname not in const_map:
                    raise ValueError(
                        f"{where}: AFIELD(radioListen value = {vname}) is not an enumerator "
                        f"of '{source.enum_info.name}' (valid: {sorted(const_map)}).")
                values.append(const_map[vname])

            behavior = _RADIO_BEHAVIORS.get(spec['behavior'].lower())
            if behavior is None:
                raise ValueError(
                    f"{where}: AFIELD(radioListen behavior = {spec['behavior']}) must be "
                    f"'grey' or 'vanish'.")

            info.source   = source_name
            info.values   = values
            info.behavior = behavior

        f.radio = info

    # Chains must be acyclic — the runtime resolves visibility by walking source
    # links, and a cycle (A follows B follows A) would never terminate. Follow
    # each listener's chain and reject a repeat.
    for start in comp.fields:
        if start.radio is None or not start.radio.source:
            continue
        seen: set = set()
        cur = start
        while cur.radio is not None and cur.radio.source:
            if cur.name in seen:
                raise ValueError(
                    f"{header_name}: AFIELD(radioListen) cycle detected in struct "
                    f"'{comp.name}' involving '{cur.name}'.")
            seen.add(cur.name)
            cur = by_name[cur.radio.source]  # existence validated above


# Field declaration: optional cv/storage-class keywords, type with optional
# namespace/template args, optional ptr/ref, name, optional default, semicolon.
# - Type modifiers (const, unsigned, etc.) can precede the base type token.
# - The pointer/ref marker may be flush against the variable name (int*foo),
#   so \s* (not \s+) separates type from name.
_FIELD_RE  = re.compile(
    r'((?:(?:const|unsigned|signed|long|short|volatile)\s+)*'  # cv/modifier keywords
    r'[\w:]+(?:\s*<[^>]*>)?'                                   # base type + optional template
    r'(?:\s*[*&])?)'                                           # optional ptr/ref
    r'\s*(\w+)'                                                # variable name (zero or more spaces after type)
    r'\s*(?:[={][^;]*)?\s*;'                                   # optional default + semicolon
)


def _extract_brace_body(text: str, start: int) -> tuple[Optional[str], int]:
    """
    Find the brace-balanced body at or after `start`.
    Returns (body_content, position_after_closing_brace).
    """
    i, n = start, len(text)
    while i < n and text[i] != '{':
        i += 1
    if i >= n:
        return None, -1
    depth = 1
    i += 1
    body_start = i
    while i < n and depth > 0:
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
        i += 1
    return text[body_start:i - 1], i


def _find_fields_in_body(body: str, source_header: str) -> list[FieldInfo]:
    """Find all AFIELD-annotated fields within a struct body."""
    fields = []
    i = 0
    while i < len(body):
        m = _AFIELD_RE.search(body, i)
        if not m:
            break
        args = parse_annot_args(m.group(1))
        rest = body[m.end():]
        fm = _FIELD_RE.match(rest.lstrip())
        if not fm:
            # A malformed AFIELD would otherwise silently drop the field — and its
            # data on every save. Fail the build loudly instead of moving on.
            snippet = ' '.join(rest.lstrip()[:60].split())
            raise ValueError(
                f"{source_header}: AFIELD is not followed by a recognisable field "
                f"declaration (got: '{snippet}...'). A reflected field must be a plain "
                f"'Type name;' declaration immediately after the AFIELD(...) macro.")
        raw_type = fm.group(1).strip()
        name     = fm.group(2).strip()
        cpp_type = raw_type.replace('const ', '').replace('*', '').replace('&', '').strip()
        fields.append(FieldInfo(name=name, cpp_type=cpp_type, args=args))
        i = m.end()
    return fields


def parse_header(path: Path) -> list[ComponentInfo]:
    """Parse a header file and return all ACOMP-annotated components.

    AENUM-annotated `enum class`es in the same header are collected first, and
    any component field whose type names one is resolved to it (enum_info),
    which is what lets reflectgen (de)serialize the field and emit its combo.
    """
    text = strip_comments(path.read_text(encoding='utf-8'))
    components: list[ComponentInfo] = []
    # Enum lookups, keyed by both the unqualified name and the fully-qualified
    # spelling so a field can reference the enum either way.
    enums: dict[str, EnumInfo] = {}

    ns_stack:       list[str] = []
    ns_open_depths: list[int] = []
    brace_depth     = 0
    i               = 0
    n               = len(text)
    pending_acomp: Optional[AnnotArgs] = None
    pending_is_asset = False

    while i < n:
        # ── Namespace ───────────────────────────────────────────────────────
        ns_m = _NS_RE.match(text, i)
        if ns_m:
            j = ns_m.end()
            while j < n and text[j] in ' \t\n\r':
                j += 1
            if j < n and text[j] == '{':
                for p in ns_m.group(1).split('::'):
                    ns_stack.append(p)
                    ns_open_depths.append(brace_depth)
                brace_depth += 1
                i = j + 1
                continue

        # ── ACOMP / AASSET / AENUM ──────────────────────────────────────────
        acomp_m = _ACOMP_RE.match(text, i)
        if acomp_m:
            pending_acomp = parse_annot_args(acomp_m.group(1))
            pending_is_asset = False
            i = acomp_m.end()
            continue
        aasset_m = _AASSET_RE.match(text, i)
        if aasset_m:
            pending_acomp = parse_annot_args(aasset_m.group(1))
            pending_is_asset = True
            i = aasset_m.end()
            continue
        aenum_m = _AENUM_RE.match(text, i)
        if aenum_m:
            # AENUM must be immediately followed (bar whitespace) by an
            # `enum class`/`enum struct` with a body — anything else is a
            # malformed annotation, which is a hard error, not a silent skip.
            j = aenum_m.end()
            while j < n and text[j] in ' \t\n\r':
                j += 1
            enum_m = _ENUM_RE.match(text, j)
            if not enum_m:
                snippet = ' '.join(text[j:j + 60].split())
                raise ValueError(
                    f"{path.name}: AENUM is not followed by an 'enum class' / 'enum struct' "
                    f"definition (got: '{snippet}...').")
            enum_name = enum_m.group(1)
            body, end = _extract_brace_body(text, enum_m.end())
            if body is None:
                raise ValueError(f"{path.name}: AENUM enum '{enum_name}' has no '{{ ... }}' body.")
            fqn = '::'.join(ns_stack + [enum_name]) if ns_stack else enum_name
            info = EnumInfo(name=enum_name, fqn=fqn, constants=parse_enum_constants(body))
            enums[enum_name] = info
            enums[fqn] = info
            i = end
            continue

        # ── Struct (only matters after ACOMP/AASSET) ────────────────────────
        if pending_acomp is not None:
            struct_m = _STRUCT_RE.match(text, i)
            if struct_m:
                name = struct_m.group(1)
                body, end = _extract_brace_body(text, struct_m.end())
                if body is not None:
                    fields = _find_fields_in_body(body, str(path))
                    components.append(ComponentInfo(
                        name=name,
                        namespaces=list(ns_stack),
                        args=pending_acomp,
                        fields=fields,
                        is_asset=pending_is_asset,
                    ))
                    pending_acomp = None
                    pending_is_asset = False
                    brace_depth += body.count('{') - body.count('}')
                    i = end
                    continue
                else:
                    pending_acomp = None
                    pending_is_asset = False

        # ── Brace / namespace tracking ───────────────────────────────────────
        ch = text[i]
        if ch == '{':
            brace_depth += 1
        elif ch == '}':
            brace_depth -= 1
            while ns_open_depths and ns_open_depths[-1] >= brace_depth:
                ns_stack.pop()
                ns_open_depths.pop()
        i += 1

    # Resolve enum-typed fields now that every AENUM in the header is known.
    for comp in components:
        for f in comp.fields:
            f.enum_info = enums.get(f.cpp_type)

    # Radio references resolve against sibling fields (and their enum_info), so
    # this must run after enum resolution. Any misuse raises here.
    for comp in components:
        _resolve_radio(comp, path.name)

    return components


# ──────────────────────────────────────────────────────────────────────────────
# Code generator
# ──────────────────────────────────────────────────────────────────────────────

def _indent(text: str, spaces: int) -> str:
    pad = ' ' * spaces
    return '\n'.join(pad + line if line.strip() else line for line in text.splitlines())


# FieldType enum values that accept AFIELD(min=/max=) bounds, mapped to the
# value range a bound may take. Floats use None ("any value"). Integer ranges
# are capped at ±2^24 — FieldMeta stores bounds as float, and that is the
# largest span where every integer is exactly representable, so a bound never
# silently shifts when it round-trips through the metadata.
NUMERIC_BOUND_RANGES: dict[str, Optional[tuple[int, int]]] = {
    'Float':  None,
    'Double': None,
    'Int':    (-2**24, 2**24),
    'Int32':  (-2**24, 2**24),
    'UInt32': (0, 2**24),
}


def _validate_bounds(f: FieldInfo, tc: Optional['TypeCodegen']) -> tuple[Optional[float], Optional[float]]:
    """Validate AFIELD(min=/max=) hints against the field's type.

    Everything wrong here is a hard generation error — a typo like
    AFIELD(min=O), a negative bound on an unsigned field, or a bound on a
    bool silently dropping the clamp is exactly the kind of quiet
    data-shape bug this generator refuses to emit.
    """
    raw_min = f.args.get('min')
    raw_max = f.args.get('max')
    if raw_min is None and raw_max is None:
        return None, None

    if tc is None or tc.enum_value not in NUMERIC_BOUND_RANGES:
        raise ValueError(f'AFIELD(min=/max=) on field "{f.name}" of type '
                         f'{f.cpp_type!r}: bounds only apply to numeric fields')

    allowed = NUMERIC_BOUND_RANGES[tc.enum_value]

    def parse(raw: Optional[str], key: str) -> Optional[float]:
        if raw is None:
            return None
        try:
            value = float(raw)
        except ValueError:
            raise ValueError(f'AFIELD({key}={raw!r}) on field "{f.name}" is not a number')
        if allowed is not None:
            lo, hi = allowed
            if not value.is_integer():
                raise ValueError(f'AFIELD({key}={raw}) on integer field "{f.name}" '
                                 f'must be a whole number')
            if not lo <= value <= hi:
                raise ValueError(f'AFIELD({key}={raw}) on field "{f.name}" is outside '
                                 f'the supported {f.cpp_type} bound range [{lo}, {hi}]')
        return value

    vmin = parse(raw_min, 'min')
    vmax = parse(raw_max, 'max')
    if vmin is not None and vmax is not None and vmin > vmax:
        raise ValueError(f'AFIELD on field "{f.name}": min={vmin:g} exceeds max={vmax:g}')
    return vmin, vmax


def _field_tc(f: FieldInfo) -> Optional['TypeCodegen']:
    """The codegen for a field. An AENUM enum synthesizes one that (de)serializes
    through its underlying integer (int64 on the wire, cast back to the enum);
    every other type comes from the TYPES table. Returns None for an unsupported
    type — the signal _check_unsupported turns into a hard error."""
    if f.enum_info is not None:
        return TypeCodegen(
            'Enum',
            'static_cast<std::int64_t>({a})',
            'if (j.contains("{f}")) {a} = static_cast<' + f.enum_info.fqn +
            '>(j.at("{f}").get<std::int64_t>());')
    return TYPES.get(f.cpp_type)


def _gen_field_meta(f: FieldInfo) -> str:
    tc        = _field_tc(f)
    ftype     = f'Assisi::Core::Reflect::FieldType::{tc.enum_value}' if tc else 'Assisi::Core::Reflect::FieldType::Unknown'
    transient = 'true' if f.args.has('transient') else 'false'
    vmin, vmax = _validate_bounds(f, tc)

    bounds_active   = vmin is not None or vmax is not None
    enum_active     = f.enum_info is not None
    listener_active = f.radio is not None and f.radio.source != ''

    # FieldMeta's trailing members are positional (bounds, then enumConstants,
    # then the radio trio), so emitting a later block forces every earlier block
    # to be emitted at its default. Blocks that no field needs are omitted, which
    # keeps unannotated fields at the short, golden-stable initializer form.
    tail: list[str] = []

    if bounds_active or enum_active or listener_active:
        has_min = 'true' if vmin is not None else 'false'
        has_max = 'true' if vmax is not None else 'false'
        min_v   = f'{vmin}f' if vmin is not None else '0.f'
        max_v   = f'{vmax}f' if vmax is not None else '0.f'
        tail += [has_min, has_max, min_v, max_v]

    if enum_active or listener_active:
        if enum_active:
            consts = ', '.join(f'{{ "{n}", {v} }}' for n, v in f.enum_info.constants)
            tail.append(f'{{ {consts} }}')
        else:
            tail.append('{}')  # empty enumConstants: a non-enum listener still needs the slot

    if listener_active:
        values = ', '.join(str(v) for v in f.radio.values)
        tail += [
            f'"{f.radio.source}"',
            f'{{ {values} }}',
            f'Assisi::Core::Reflect::RadioBehavior::{f.radio.behavior}',
        ]

    base = f'{{ "{f.name}", {ftype}, offsetof(T, {f.name}), {transient}'
    return base + ' }' if not tail else base + ', ' + ', '.join(tail) + ' }'


def _is_serializable(f: FieldInfo) -> bool:
    """A non-transient field reflectgen has codegen for (a TYPES entry or an enum)."""
    return not f.args.has('transient') and _field_tc(f) is not None


def _gen_serialize(fields: list[FieldInfo]) -> str:
    # Default-deny (enforced in generate_cpp) guarantees every non-transient
    # field has codegen, so there is no unsupported branch to emit.
    serializable = [f for f in fields if _is_serializable(f)]

    if not serializable:
        # Nothing to serialize — suppress unused-parameter warning.
        return '(void)ptr;\nreturn nlohmann::json{};'

    lines = ['const auto& c = *static_cast<const T*>(ptr);', 'return nlohmann::json{']
    for f in serializable:
        expr = _field_tc(f).serialize.format(a=f'c.{f.name}', f=f.name)
        lines.append(f'    {{ "{f.name}", {expr} }},')
    lines.append('};')
    return '\n'.join(lines)


def _gen_deserialize(fields: list[FieldInfo]) -> str:
    serializable = [f for f in fields if _is_serializable(f)]

    lines = [
        'auto& scene = *static_cast<Assisi::ECS::Scene*>(scene_ptr);',
        'Assisi::ECS::Entity e{entity_index, entity_gen};',
        'T comp{};',
    ]

    if not serializable:
        lines.append('(void)j;')
    else:
        for f in serializable:
            lines.append(_field_tc(f).deserialize.format(f=f.name, a=f'comp.{f.name}'))

    lines.append('(void)scene.Add(e, comp);')
    return '\n'.join(lines)


def _gen_deserialize_asset(fields: list[FieldInfo]) -> str:
    """Deserialize for an AASSET: write fields into a caller-owned instance
    (out_ptr), no scene/entity machinery. Per-field 'if present' so absent keys
    leave the instance's current value untouched (forward-compat)."""
    serializable = [f for f in fields if _is_serializable(f)]

    if not serializable:
        return '(void)j;\n(void)out_ptr;'

    lines = ['auto& a = *static_cast<T*>(out_ptr);']
    for f in serializable:
        lines.append(_field_tc(f).deserialize.format(f=f.name, a=f'a.{f.name}'))
    return '\n'.join(lines)


# EntityRef is meaningless in a standalone asset (there is no scene to resolve
# a serial index against), and its codegen references Runtime::SceneSerializer,
# which an asset's home module (e.g. Geometry) does not link. Forbid it.
_ENTITY_REF_TYPES = {'ECS::Entity', 'Assisi::ECS::Entity'}


def _check_asset_fields(components: list[ComponentInfo], header_name: str) -> None:
    for comp in components:
        if not comp.is_asset:
            continue
        for f in comp.fields:
            if f.args.has('transient'):
                continue
            if f.cpp_type in _ENTITY_REF_TYPES:
                raise ValueError(
                    f"{header_name}: asset '{comp.name}' field '{f.name}' is an "
                    f"EntityRef, which AASSET types cannot serialize (no scene to "
                    f"resolve against). Remove it or mark it AFIELD(transient).")


def _gen_asset_block(comp: ComponentInfo) -> str:
    fqn      = '::'.join(comp.namespaces + [comp.name]) if comp.namespaces else comp.name
    var_name = f'_reflectgen_{comp.name}'
    field_metas = ',\n            '.join(_gen_field_meta(f) for f in comp.fields)
    serialize   = _indent(_gen_serialize(comp.fields), 12)
    deserialize = _indent(_gen_deserialize_asset(comp.fields), 12)

    return f"""\
// ── {comp.name} {'─' * max(0, 74 - len(comp.name))}
// AASSET: standalone asset type, registered with AssetTypeRegistry.
static const bool {var_name} = []() -> bool
{{
    using T = {fqn};
    Assisi::Core::Reflect::AssetTypeRegistry::Instance().Register({{
        "{comp.name}",
        typeid(T),
        {{
            {field_metas}
        }},
        [](const void* ptr) -> nlohmann::json
        {{
{serialize}
        }},
        [](const nlohmann::json& j, void* out_ptr)
        {{
{deserialize}
        }},
    }});
    return true;
}}();

"""


def generate_cpp(components: list[ComponentInfo], include_path: str) -> str:
    # Default-deny is enforced here (not only in main) so every path that emits
    # code — the CLI and direct callers such as the golden tests — refuses an
    # unserializable field rather than silently dropping it.
    _check_unsupported(components, include_path)
    _check_asset_fields(components, include_path)

    component_infos = [c for c in components if not c.is_asset]
    asset_infos     = [c for c in components if c.is_asset]

    has_entity_refs = any(
        f.cpp_type in _ENTITY_REF_TYPES
        for comp in component_infos
        if not comp.args.has('transient')  # id-only components serialize nothing
        for f in comp.fields
        if not f.args.has('transient')
    )

    # AssetId fields (in components or assets) call the Core AssetId JSON helpers,
    # so a generated file that has any must include their header.
    has_asset_ids = any(
        f.cpp_type in _ASSET_ID_TYPES
        for comp in components
        if not comp.args.has('transient')
        for f in comp.fields
        if not f.args.has('transient')
    )

    # Enum fields (de)serialize through a std::int64_t cast, which needs <cstdint>.
    has_enums = any(
        f.enum_info is not None
        for comp in components
        if not comp.args.has('transient')
        for f in comp.fields
        if not f.args.has('transient')
    )

    # Includes are conditional on what the header actually declares. An
    # asset-only header (e.g. Geometry's MaterialData) must NOT pull in
    # ComponentRegistry / ECS::Scene — its home module does not link ECS.
    includes = []
    if component_infos:
        includes.append('#include <Assisi/Core/Reflect/ComponentRegistry.hpp>')
        includes.append('#include <Assisi/ECS/Scene.hpp>')
        if has_entity_refs:
            includes.append('#include <Assisi/Runtime/SceneSerializer.hpp>')
    if asset_infos:
        includes.append('#include <Assisi/Core/Reflect/AssetTypeRegistry.hpp>')
    if has_asset_ids:
        includes.append('#include <Assisi/Core/AssetIdJson.hpp>')
    if has_enums:
        includes.append('#include <cstdint>')
    includes.append(f'#include <{include_path}>')
    include_block = '\n'.join(includes)

    blocks = []
    blocks.append(f"""\
// AUTO-GENERATED by reflectgen — do not edit.
// Source: {include_path}

{include_block}

namespace
{{
""")

    for comp in component_infos:
        fqn      = '::'.join(comp.namespaces + [comp.name]) if comp.namespaces else comp.name
        var_name = f'_reflectgen_{comp.name}'

        # ACOMP(tracked): opt this component into change detection by emitting a
        # trailing tracksChanges arg. Omitted otherwise so the field defaults to
        # false and the golden output for untracked components is unchanged. The
        # serializable literal grows a trailing comma (on the code side, before
        # its // comment) when the extra arg follows it.
        _tracked   = comp.args.has('tracked')
        _tail      = '\n        true       // tracksChanges' if _tracked else ''
        serial_yes = ('true,' if _tracked else 'true').ljust(11) + '// serializable' + _tail
        serial_no  = ('false,' if _tracked else 'false').ljust(11) + '// serializable' + _tail

        # ACOMP(transient): register only to receive a stable ComponentId (so a
        # Scene can store the type) with no serialization hooks. serializable is
        # false and every hook is null; consumers gate on ComponentMeta::
        # serializable. See RigidBody / DestroyTag.
        if comp.args.has('transient'):
            blocks.append(f"""\
// ── {comp.name} {'─' * max(0, 74 - len(comp.name))}
// ACOMP(transient): id-only registration, not serialized.
static const bool {var_name} = []() -> bool
{{
    using T = {fqn};
    Assisi::Core::Reflect::ComponentRegistry::Instance().Register({{
        "{comp.name}",
        typeid(T),
        {{}},      // fields: none reflected
        nullptr,   // serialize
        nullptr,   // addToScene
        nullptr,   // iterateEntities
        nullptr,   // getByEntity
        {serial_no}
    }});
    return true;
}}();

""")
            continue

        field_metas = ',\n            '.join(_gen_field_meta(f) for f in comp.fields)
        serialize   = _indent(_gen_serialize(comp.fields), 12)
        deserialize = _indent(_gen_deserialize(comp.fields), 12)

        blocks.append(f"""\
// ── {comp.name} {'─' * max(0, 74 - len(comp.name))}
static const bool {var_name} = []() -> bool
{{
    using T = {fqn};
    Assisi::Core::Reflect::ComponentRegistry::Instance().Register({{
        "{comp.name}",
        typeid(T),
        {{
            {field_metas}
        }},
        [](const void* ptr) -> nlohmann::json
        {{
{serialize}
        }},
        [](void* scene_ptr, uint32_t entity_index, uint32_t entity_gen, const nlohmann::json& j)
        {{
{deserialize}
        }},
        [](void* scene_ptr, std::function<void(uint32_t, uint32_t, const void*)> cb)
        {{
            auto& scene = *static_cast<Assisi::ECS::Scene*>(scene_ptr);
            for (auto [e, comp] : scene.Query<T>())
                cb(e.index, e.generation, &comp);
        }},
        [](void* scene_ptr, uint32_t entity_index, uint32_t entity_gen) -> const void*
        {{
            auto& scene = *static_cast<Assisi::ECS::Scene*>(scene_ptr);
            return scene.Get<T>(Assisi::ECS::Entity{{entity_index, entity_gen}});
        }},
        {serial_yes}
    }});
    return true;
}}();

""")

    for comp in asset_infos:
        blocks.append(_gen_asset_block(comp))

    blocks.append('} // namespace\n')
    return ''.join(blocks)


# ──────────────────────────────────────────────────────────────────────────────
# Include-path auto-detection
# ──────────────────────────────────────────────────────────────────────────────

def _detect_include_path(header: Path) -> str:
    """
    If the path contains an 'include' segment, return everything after it.
    e.g. .../modules/Runtime/include/Assisi/Runtime/Foo.hpp -> Assisi/Runtime/Foo.hpp
    Otherwise return just the filename.
    """
    parts = header.parts
    try:
        idx = next(i for i, p in enumerate(parts) if p.lower() == 'include')
        return '/'.join(parts[idx + 1:])
    except StopIteration:
        return header.name


def _check_unsupported(components: list[ComponentInfo], header_name: str) -> None:
    """Default-deny: fail generation if any non-transient AFIELD has a type
    reflectgen cannot (de)serialize (i.e. absent from TYPES).

    A silently-skipped field round-trips to nothing — every save drops it —
    which is far worse than a build error. Mark the field AFIELD(transient) if
    it is runtime-only, or add its codegen to TYPES. UNSUPPORTED_TYPES supplies
    a richer reason string when one is known.
    """
    for comp in components:
        if comp.args.has('transient'):
            continue  # id-only component: its fields are never serialized
        for f in comp.fields:
            if f.args.has('transient'):
                continue
            if _field_tc(f) is None:
                reason = UNSUPPORTED_TYPES.get(f.cpp_type, 'no codegen for this type')
                raise ValueError(
                    f"{header_name}: field '{comp.name}::{f.name}' has type "
                    f"'{f.cpp_type}', which reflectgen cannot serialize ({reason}). "
                    f"Add its codegen to TYPES, mark it AFIELD(transient), or (for an "
                    f"enum class) annotate it AENUM().")


# ──────────────────────────────────────────────────────────────────────────────
# Entry point
# ──────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description='Assisi reflection code generator')
    parser.add_argument('headers', nargs='+', type=Path,
                        help='Header file(s) to process')
    parser.add_argument('--outdir', type=Path, required=True,
                        help='Output directory for .generated.cpp files')
    parser.add_argument('--include', dest='include_path', default=None,
                        help='Override #include path in generated file '
                             '(auto-detected from include/ segment if omitted)')
    args = parser.parse_args()

    args.outdir.mkdir(parents=True, exist_ok=True)

    ok = True
    for header in args.headers:
        header = header.resolve()
        include_path = args.include_path or _detect_include_path(header)

        print(f'reflectgen: {header.name} -> {include_path}')
        try:
            components = parse_header(header)
            _check_unsupported(components, header.name)
        except Exception as e:
            print(f'  error: {e}', file=sys.stderr)
            ok = False
            continue

        if not components:
            print(f'  (no ACOMP annotations found, skipping)')
            continue

        for comp in components:
            print(f'  found: {comp.name} ({len(comp.fields)} field(s))')

        cpp = generate_cpp(components, include_path)
        out = args.outdir / (header.stem + '.generated.cpp')
        out.write_text(cpp, encoding='utf-8')
        print(f'  wrote: {out}')

    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()