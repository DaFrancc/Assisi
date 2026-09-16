#!/usr/bin/env python3
"""Fail if an ELF binary needs a shared library outside an allow-list.

A shipped game has to start on a machine that has only an operating system and
a graphics driver. Every library named in its dynamic section is one the player
must already have, in a compatible version, and the way this regresses is a new
dependency quietly linking against the build machine's copy of something.

Reads DT_NEEDED with readelf rather than running ldd: ldd resolves the whole
chain on this machine and can execute the binary's loader, while DT_NEEDED is
exactly what the binary itself asks for.

Exit codes: 0 every needed library is allowed, 1 one is not, SKIP_EXIT_CODE the
check could not run (no readelf, or not an ELF binary).
"""

import argparse
import re
import shutil
import subprocess
import sys

# ctest reads this back through SKIP_RETURN_CODE. Must match the symbol scan's.
SKIP_EXIT_CODE = 77

NEEDED_PATTERN = re.compile(r"\(NEEDED\)\s+Shared library: \[(?P<name>[^\]]+)\]")


def needed_libraries(readelf: str, binary: str) -> list[str]:
    """The shared libraries `binary` names in its dynamic section."""
    result = subprocess.run([readelf, "-d", binary], capture_output=True, text=True, check=False)
    if result.returncode != 0:
        print(f"check-dependencies: {readelf} failed on {binary}: {result.stderr.strip()}", file=sys.stderr)
        sys.exit(SKIP_EXIT_CODE)
    return [match.group("name") for match in NEEDED_PATTERN.finditer(result.stdout)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--allow", action="append", default=[],
                        help="a shared library the binary may need, by soname")
    args = parser.parse_args()

    if shutil.which(args.readelf) is None:
        print(f"check-dependencies: {args.readelf} not found", file=sys.stderr)
        return SKIP_EXIT_CODE
    if not args.allow:
        print("check-dependencies: no --allow given, so nothing is proven", file=sys.stderr)
        return SKIP_EXIT_CODE

    needed = needed_libraries(args.readelf, args.binary)
    disallowed = [name for name in needed if name not in args.allow]
    for name in disallowed:
        print(f"check-dependencies: {args.binary} needs {name}, which a player may not have", file=sys.stderr)
    if disallowed:
        return 1

    print(f"check-dependencies: {len(needed)} needed, all allowed: {', '.join(needed)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
