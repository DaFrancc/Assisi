#!/usr/bin/env python3
"""Fail if a binary defines a symbol it is not allowed to contain.

The Game executable must not carry the editor. Linking is what enforces that,
and this is what proves the link stayed right: a convenient include added months
from now pulls a library back in without anyone noticing, and the symptom is a
player unpacking a level editor.

Matching is a plain substring test against demangled names, because the thing
being excluded is a whole namespace or library rather than a particular
function. `Assisi::Editor::` catches every member of it, present and future.

Exit codes: 0 clean, 1 a forbidden symbol is present, SKIP_EXIT_CODE the scan
could not run (see below). Never 0 for a scan that did not happen -- a stripped
binary has no symbols at all, and reporting that as clean would turn this into a
test that passes hardest exactly where it is needed most.
"""

import argparse
import shutil
import subprocess
import sys

# ctest reads this back through SKIP_RETURN_CODE and marks the test skipped
# rather than passed or failed. Distinct from 0 and 1 so an unrunnable scan can
# never be mistaken for a clean one.
SKIP_EXIT_CODE = 77

# How many offending symbols to print before summarising. A binary that links
# the whole of ImGui defines thousands of them, and a wall of mangled names
# buries the one line saying which library came back.
MAX_REPORTED_PER_PATTERN = 5


def read_symbols(nm_tool: str, binary: str) -> list[str]:
    """Every symbol `binary` defines, demangled.

    --defined-only excludes undefined references, which name things the binary
    calls rather than things it contains: a header-only helper that mentions a
    type is not the same as linking its implementation.
    """
    result = subprocess.run(
        [nm_tool, "-C", "--defined-only", binary],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        stderr = result.stderr.strip()
        # "no symbols" is how nm reports a stripped binary, and it arrives as a
        # failure rather than as empty output.
        if "no symbols" in stderr.lower():
            return []
        print(f"check-symbols: {nm_tool} failed on {binary}: {stderr}", file=sys.stderr)
        sys.exit(SKIP_EXIT_CODE)
    return result.stdout.splitlines()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="the executable to scan")
    parser.add_argument(
        "--forbid",
        action="append",
        default=[],
        metavar="PATTERN",
        help="substring no demangled symbol name may contain; repeatable",
    )
    parser.add_argument("--nm", default="nm", help="the nm to run")
    args = parser.parse_args()

    if not args.forbid:
        print("check-symbols: no --forbid patterns given, so nothing is proven",
              file=sys.stderr)
        return SKIP_EXIT_CODE

    nm_tool = shutil.which(args.nm)
    if nm_tool is None:
        print(f"check-symbols: no '{args.nm}' on PATH; cannot read a symbol table",
              file=sys.stderr)
        return SKIP_EXIT_CODE

    symbols = read_symbols(nm_tool, args.binary)
    if not symbols:
        print(f"check-symbols: {args.binary} exposes no symbols -- stripped, or not "
              f"an object file. Nothing was checked.", file=sys.stderr)
        return SKIP_EXIT_CODE

    found = {}
    for pattern in args.forbid:
        matches = [line for line in symbols if pattern in line]
        if matches:
            found[pattern] = matches

    if not found:
        print(f"check-symbols: {args.binary} is clean against "
              f"{len(args.forbid)} pattern(s), {len(symbols)} symbols scanned.")
        return 0

    print(f"check-symbols: {args.binary} contains symbols it must not:",
          file=sys.stderr)
    for pattern, matches in found.items():
        print(f"\n  '{pattern}' -- {len(matches)} symbol(s):", file=sys.stderr)
        for line in matches[:MAX_REPORTED_PER_PATTERN]:
            print(f"      {line.strip()}", file=sys.stderr)
        if len(matches) > MAX_REPORTED_PER_PATTERN:
            remaining = len(matches) - MAX_REPORTED_PER_PATTERN
            print(f"      ... and {remaining} more", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
