#!/usr/bin/env python3
"""reflect_parser — header scanning and the parse-time data model for reflectgen.

Turns a C++ header into a list[ComponentInfo]: strips comments, tracks the
namespace stack, extracts ACOMP/AASSET/AENUM/AFIELD annotations, and resolves
enum-typed fields and the AFIELD(radio ...) references (with cycle detection).
Everything here is about *understanding* the header; nothing emits C++ (that is
reflect_codegen). The dataclasses below are the contract between the two.
"""

import re
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
    size:      int  = 4            # underlying byte width: 1/2/4/8
    is_signed: bool = True         # underlying type is signed (default `int` is)


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
# `enum class Name` / `enum struct Name`, with an optional `: underlying`. Both
# the name (group 1) and the underlying-type spelling (group 2, up to the body's
# `{`) are captured; the brace body is extracted separately.
_ENUM_RE   = re.compile(r'\benum\s+(?:class|struct)\s+(\w+)\s*(?::\s*([^{]+))?')
_NS_RE     = re.compile(r'\bnamespace\s+([\w:]+)')

# Underlying-type spelling → (byte width, is_signed). Whitespace is normalised to
# single spaces before lookup. Only fixed-width types and the `int`/`unsigned`
# defaults are supported; platform-dependent `long`/`unsigned long` are omitted
# on purpose so their ambiguous width fails the build (see _enum_underlying).
_ENUM_UNDERLYING: dict[str, tuple[int, bool]] = {
    'char':                   (1, True),   # impl-defined signedness; treat as signed
    'signed char':            (1, True),
    'unsigned char':          (1, False),
    'int8_t':                 (1, True),  'std::int8_t':  (1, True),
    'uint8_t':                (1, False), 'std::uint8_t': (1, False),
    'short':                  (2, True),  'short int':    (2, True),  'signed short': (2, True),
    'unsigned short':         (2, False), 'unsigned short int': (2, False),
    'int16_t':                (2, True),  'std::int16_t': (2, True),
    'uint16_t':               (2, False), 'std::uint16_t': (2, False),
    'int':                    (4, True),  'signed':       (4, True),  'signed int':   (4, True),
    'unsigned':               (4, False), 'unsigned int': (4, False),
    'int32_t':                (4, True),  'std::int32_t': (4, True),
    'uint32_t':               (4, False), 'std::uint32_t': (4, False),
    'long long':              (8, True),  'signed long long': (8, True), 'long long int': (8, True),
    'unsigned long long':     (8, False), 'unsigned long long int': (8, False),
    'int64_t':                (8, True),  'std::int64_t': (8, True),
    'uint64_t':               (8, False), 'std::uint64_t': (8, False),
}


def _enum_underlying(underlying: Optional[str], enum_name: str, header_name: str) -> tuple[int, bool]:
    """Map an `enum class` underlying-type spelling to (byte width, is_signed).

    None means no `: type` was given, so the C++ default underlying for a scoped
    enum applies: `int` (4-byte signed). An unrecognised spelling — including the
    platform-dependent `long` — is a hard build error, since the editor must know
    the exact width to read/write the field without corrupting neighbours."""
    if underlying is None:
        return 4, True
    key = ' '.join(underlying.split())
    if key not in _ENUM_UNDERLYING:
        raise ValueError(
            f"{header_name}: AENUM enum '{enum_name}' has underlying type '{key}', which "
            f"reflectgen does not support. Use a fixed-width integer type (int8_t, uint16_t, "
            f"int32_t, uint64_t, ...) or the default `int`; 'long' is platform-dependent.")
    return _ENUM_UNDERLYING[key]


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


def _resolve_radio(comp: ComponentInfo, header_name: str) -> None:
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
            size, is_signed = _enum_underlying(enum_m.group(2), enum_name, path.name)
            body, end = _extract_brace_body(text, enum_m.end())
            if body is None:
                raise ValueError(f"{path.name}: AENUM enum '{enum_name}' has no '{{ ... }}' body.")
            fqn = '::'.join(ns_stack + [enum_name]) if ns_stack else enum_name
            info = EnumInfo(name=enum_name, fqn=fqn, constants=parse_enum_constants(body),
                            size=size, is_signed=is_signed)
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
