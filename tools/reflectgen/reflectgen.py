#!/usr/bin/env python3
"""reflectgen.py — Assisi Reflection Code Generator (CLI entry point)

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

The implementation is split across three sibling modules; this file is the CLI
plus a stable facade that re-exports their public surface so `import reflectgen`
keeps working for callers and tests:

    reflect_types    the C++ type -> codegen mapping (TYPES, TypeCodegen, ...)
    reflect_parser   header scanning + the parse-time data model
    reflect_codegen  emission of the .generated.cpp registration code
"""

import sys
import argparse
from pathlib import Path

# Re-export the public surface so `reflectgen.X` resolves for every existing
# caller and the test-suite (which reaches in for parse_header, generate_cpp,
# the dataclasses, UNSUPPORTED_TYPES, _check_unsupported, _detect_include_path).
from reflect_types import (  # noqa: F401
    TypeCodegen,
    TYPES,
    UNSUPPORTED_TYPES,
    _ASSET_ID_TYPES,
    _ENTITY_REF_TYPES,
)
from reflect_parser import (  # noqa: F401
    AnnotArgs,
    EnumInfo,
    RadioInfo,
    FieldInfo,
    ComponentInfo,
    parse_header,
    parse_annot_args,
    parse_radio_spec,
    parse_enum_constants,
    strip_comments,
    _detect_include_path,
)
from reflect_codegen import (  # noqa: F401
    generate_cpp,
    _check_unsupported,
    _check_asset_fields,
    _field_tc,
    NUMERIC_BOUND_RANGES,
)


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
