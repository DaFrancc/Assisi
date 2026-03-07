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
class FieldInfo:
    name:     str
    cpp_type: str
    args:     AnnotArgs


@dataclass
class ComponentInfo:
    name:       str
    namespaces: list   # e.g. ['Assisi', 'Runtime']
    args:       AnnotArgs
    fields:     list   # list[FieldInfo]


# ──────────────────────────────────────────────────────────────────────────────
# Type → codegen mapping
# ──────────────────────────────────────────────────────────────────────────────

@dataclass
class TypeCodegen:
    enum_value:  str  # Core::Reflect::FieldType::Xxx
    serialize:   str  # expression; {a} = member accessor e.g. "c.foo"
    deserialize: str  # statement;  {f} = field name, {a} = accessor


# Serialize expressions produce values for json initializer lists.
# Deserialize statements read from j.at("{f}") and assign to comp.{f}.
#
# GLM quat: memory layout {x,y,z,w}, constructor glm::quat(w,x,y,z).
# We serialize as [w,x,y,z] to match math convention.
TYPES: dict[str, TypeCodegen] = {
    'float':     TypeCodegen(
        'Float',
        '{a}',
        'comp.{f} = j.at("{f}").get<float>();'),
    'double':    TypeCodegen(
        'Double',
        '{a}',
        'comp.{f} = j.at("{f}").get<double>();'),
    'int':       TypeCodegen(
        'Int',
        '{a}',
        'comp.{f} = j.at("{f}").get<int>();'),
    'int32_t':   TypeCodegen(
        'Int32',
        '{a}',
        'comp.{f} = j.at("{f}").get<int32_t>();'),
    'uint32_t':  TypeCodegen(
        'UInt32',
        '{a}',
        'comp.{f} = j.at("{f}").get<uint32_t>();'),
    'bool':      TypeCodegen(
        'Bool',
        '{a}',
        'comp.{f} = j.at("{f}").get<bool>();'),
    'glm::vec2': TypeCodegen(
        'Vec2',
        '{{ {a}.x, {a}.y }}',
        '{{ const auto& _v = j.at("{f}"); comp.{f} = {{ _v[0].get<float>(), _v[1].get<float>() }}; }}'),
    'glm::vec3': TypeCodegen(
        'Vec3',
        '{{ {a}.x, {a}.y, {a}.z }}',
        '{{ const auto& _v = j.at("{f}"); comp.{f} = {{ _v[0].get<float>(), _v[1].get<float>(), _v[2].get<float>() }}; }}'),
    'glm::vec4': TypeCodegen(
        'Vec4',
        '{{ {a}.x, {a}.y, {a}.z, {a}.w }}',
        '{{ const auto& _v = j.at("{f}"); comp.{f} = {{ _v[0].get<float>(), _v[1].get<float>(), _v[2].get<float>(), _v[3].get<float>() }}; }}'),
    'glm::quat': TypeCodegen(
        'Quat',
        '{{ {a}.w, {a}.x, {a}.y, {a}.z }}',
        '{{ const auto& _v = j.at("{f}"); comp.{f} = glm::quat{{ _v[0].get<float>(), _v[1].get<float>(), _v[2].get<float>(), _v[3].get<float>() }}; }}'),
    'glm::mat4': TypeCodegen(
        'Mat4',
        'nullptr /* TODO: mat4 serialize */',
        '/* TODO: mat4 deserialize for field {f} */'),
}


# ──────────────────────────────────────────────────────────────────────────────
# Annotation argument parser
# ──────────────────────────────────────────────────────────────────────────────

def _split_args(s: str) -> list[str]:
    """Split by comma, respecting quoted strings."""
    result, current, in_quote, qchar = [], [], False, ''
    for ch in s:
        if in_quote:
            current.append(ch)
            if ch == qchar:
                in_quote = False
        elif ch in ('"', "'"):
            in_quote, qchar = True, ch
            current.append(ch)
        elif ch == ',':
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
_AFIELD_RE = re.compile(r'\bAFIELD\s*\(([^)]*)\)')
_STRUCT_RE = re.compile(r'\bstruct\s+(\w+)')
_NS_RE     = re.compile(r'\bnamespace\s+([\w:]+)')
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
        if fm:
            raw_type = fm.group(1).strip()
            name     = fm.group(2).strip()
            cpp_type = raw_type.replace('const ', '').replace('*', '').replace('&', '').strip()
            fields.append(FieldInfo(name=name, cpp_type=cpp_type, args=args))
        else:
            print(f'  warning: AFIELD not followed by a recognisable field declaration '
                  f'in {source_header}', file=sys.stderr)
        i = m.end()
    return fields


def parse_header(path: Path) -> list[ComponentInfo]:
    """Parse a header file and return all ACOMP-annotated components."""
    text = strip_comments(path.read_text(encoding='utf-8'))
    components: list[ComponentInfo] = []

    ns_stack:       list[str] = []
    ns_open_depths: list[int] = []
    brace_depth     = 0
    i               = 0
    n               = len(text)
    pending_acomp: Optional[AnnotArgs] = None

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

        # ── ACOMP ───────────────────────────────────────────────────────────
        acomp_m = _ACOMP_RE.match(text, i)
        if acomp_m:
            pending_acomp = parse_annot_args(acomp_m.group(1))
            i = acomp_m.end()
            continue

        # ── Struct (only matters after ACOMP) ───────────────────────────────
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
                    ))
                    pending_acomp = None
                    brace_depth += body.count('{') - body.count('}')
                    i = end
                    continue
                else:
                    pending_acomp = None

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

    return components


# ──────────────────────────────────────────────────────────────────────────────
# Code generator
# ──────────────────────────────────────────────────────────────────────────────

def _indent(text: str, spaces: int) -> str:
    pad = ' ' * spaces
    return '\n'.join(pad + line if line.strip() else line for line in text.splitlines())


def _gen_field_meta(f: FieldInfo) -> str:
    tc        = TYPES.get(f.cpp_type)
    ftype     = f'Assisi::Core::Reflect::FieldType::{tc.enum_value}' if tc else 'Assisi::Core::Reflect::FieldType::Unknown'
    transient = 'true' if f.args.has('transient') else 'false'
    return f'{{ "{f.name}", {ftype}, offsetof(T, {f.name}), {transient} }}'


def _gen_serialize(fields: list[FieldInfo]) -> str:
    serializable = [f for f in fields if not f.args.has('transient') and TYPES.get(f.cpp_type)]
    unsupported  = [f for f in fields if not f.args.has('transient') and not TYPES.get(f.cpp_type)]

    if not serializable and not unsupported:
        # Nothing to serialize — suppress unused-parameter warning.
        return '(void)ptr;\nreturn nlohmann::json{};'

    lines = ['const auto& c = *static_cast<const T*>(ptr);', 'return nlohmann::json{']
    for f in unsupported:
        lines.append(f'    /* WARNING: unsupported type \'{f.cpp_type}\' for field \'{f.name}\' — skipped */')
    for f in serializable:
        expr = TYPES[f.cpp_type].serialize.format(a=f'c.{f.name}', f=f.name)
        lines.append(f'    {{ "{f.name}", {expr} }},')
    lines.append('};')
    return '\n'.join(lines)


def _gen_deserialize(fields: list[FieldInfo]) -> str:
    serializable = [f for f in fields if not f.args.has('transient') and TYPES.get(f.cpp_type)]
    unsupported  = [f for f in fields if not f.args.has('transient') and not TYPES.get(f.cpp_type)]

    lines = [
        'auto& scene = *static_cast<Assisi::ECS::Scene*>(scene_ptr);',
        'Assisi::ECS::Entity e{entity_index, entity_gen};',
        'T comp{};',
    ]

    if not serializable and not unsupported:
        lines.append('(void)j;')
    else:
        for f in unsupported:
            lines.append(f'/* WARNING: unsupported type \'{f.cpp_type}\' for field \'{f.name}\' — skipped */')
        for f in serializable:
            lines.append(TYPES[f.cpp_type].deserialize.format(f=f.name, a=f'comp.{f.name}'))

    lines.append('(void)scene.Add(e, comp);')
    return '\n'.join(lines)


def generate_cpp(components: list[ComponentInfo], include_path: str) -> str:
    blocks = []
    blocks.append(f"""\
// AUTO-GENERATED by reflectgen — do not edit.
// Source: {include_path}

#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <{include_path}>

namespace
{{
""")

    for comp in components:
        fqn         = '::'.join(comp.namespaces + [comp.name]) if comp.namespaces else comp.name
        var_name    = f'_reflectgen_{comp.name}'
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
        }}
    }});
    return true;
}}();

""")

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