#!/usr/bin/env python3
"""Fail if an ELF binary needs a shared library outside an allow-list, or a
glibc newer than a given ceiling.

A shipped game has to start on a machine that has only an operating system and
a graphics driver. Every library named in its dynamic section is one the player
must already have, in a compatible version, and the way this regresses is a new
dependency quietly linking against the build machine's copy of something.

glibc is the one library the game cannot avoid, and a binary runs on the glibc
it was built against or newer. Each glibc function it calls carries the version
that introduced it, so the newest version the binary names is the oldest glibc
it starts on. --max-glibc refuses anything newer, for a build that promises to
run on an older distro than the one it was built on.

Reads DT_NEEDED and the version needs with readelf rather than running ldd: ldd
resolves the whole chain on this machine and can execute the binary's loader,
while those sections are exactly what the binary itself asks for.

Exit codes: 0 every check passed, 1 one did not, SKIP_EXIT_CODE the check could
not run (no readelf, no binary, or output it could not read).
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

# ctest reads this back through SKIP_RETURN_CODE. Must match the symbol scan's.
SKIP_EXIT_CODE = 77

NEEDED_PATTERN = re.compile(r"\(NEEDED\)\s+Shared library: \[(?P<name>[^\]]+)\]")

# readelf -V lists the version needs grouped under the library that provides
# them. Only numbered GLIBC_ versions are versions: GLIBC_PRIVATE and the
# GLIBC_ABI_ markers name no release.
VERSION_FILE_PATTERN = re.compile(r"File: (?P<library>\S+)")
GLIBC_VERSION_PATTERN = re.compile(r"Name: GLIBC_(?P<version>\d+(?:\.\d+)+)\b")
CEILING_PATTERN = re.compile(r"^\d+(?:\.\d+)+$")


def run_readelf(readelf: str, flags: list[str], binary: str) -> str:
    """readelf's output for `binary`, or a skip when it cannot be read."""
    result = subprocess.run([readelf, *flags, binary], capture_output=True, text=True, check=False)
    if result.returncode != 0:
        print(f"check-dependencies: {readelf} failed on {binary}: {result.stderr.strip()}", file=sys.stderr)
        sys.exit(SKIP_EXIT_CODE)
    return result.stdout


def needed_libraries(readelf: str, binary: str) -> list[str]:
    """The shared libraries `binary` names in its dynamic section."""
    return [match.group("name") for match in NEEDED_PATTERN.finditer(run_readelf(readelf, ["-d"], binary))]


def glibc_needs(readelf: str, binary: str) -> list[tuple[str, str]]:
    """Each (library, version) pair of glibc versions `binary` needs."""
    needs = []
    library = ""
    for line in run_readelf(readelf, ["-V", "-W"], binary).splitlines():
        file_match = VERSION_FILE_PATTERN.search(line)
        if file_match:
            library = file_match.group("library")
            continue
        version_match = GLIBC_VERSION_PATTERN.search(line)
        if version_match:
            needs.append((library, version_match.group("version")))
    return needs


def version_key(version: str) -> tuple[int, ...]:
    """A version as numbers, so 2.9 sorts below 2.31."""
    return tuple(int(part) for part in version.split("."))


def check_libraries(readelf: str, binary: str, allowed: list[str]) -> bool:
    needed = needed_libraries(readelf, binary)
    disallowed = [name for name in needed if name not in allowed]
    for name in disallowed:
        print(f"check-dependencies: {binary} needs {name}, which a player may not have", file=sys.stderr)
    if disallowed:
        return False
    print(f"check-dependencies: {len(needed)} needed, all allowed: {', '.join(needed)}")
    return True


def check_glibc(readelf: str, binary: str, ceiling: str) -> bool:
    needs = glibc_needs(readelf, binary)
    if not needs:
        print(f"check-dependencies: found no glibc version needs in {binary}, so the ceiling proves nothing",
              file=sys.stderr)
        sys.exit(SKIP_EXIT_CODE)

    too_new = sorted({(library, version) for library, version in needs
                      if version_key(version) > version_key(ceiling)},
                     key=lambda need: version_key(need[1]))
    for library, version in too_new:
        print(f"check-dependencies: {binary} needs GLIBC_{version} from {library}, newer than the "
              f"GLIBC_{ceiling} it has to run on", file=sys.stderr)
    if too_new:
        return False

    newest = max((version for _, version in needs), key=version_key)
    print(f"check-dependencies: newest glibc needed is GLIBC_{newest}, within GLIBC_{ceiling}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--allow", action="append", default=[],
                        help="a shared library the binary may need, by soname")
    parser.add_argument("--max-glibc", help="the newest glibc version the binary may need, e.g. 2.31")
    args = parser.parse_args()

    if args.max_glibc is not None and not CEILING_PATTERN.match(args.max_glibc):
        parser.error(f"--max-glibc takes a version such as 2.31, not '{args.max_glibc}'")
    if shutil.which(args.readelf) is None:
        print(f"check-dependencies: {args.readelf} not found", file=sys.stderr)
        return SKIP_EXIT_CODE
    if not Path(args.binary).is_file():
        print(f"check-dependencies: {args.binary} has not been built", file=sys.stderr)
        return SKIP_EXIT_CODE
    if not args.allow:
        print("check-dependencies: no --allow given, so nothing is proven", file=sys.stderr)
        return SKIP_EXIT_CODE

    libraries_ok = check_libraries(args.readelf, args.binary, args.allow)
    glibc_ok = args.max_glibc is None or check_glibc(args.readelf, args.binary, args.max_glibc)
    return 0 if libraries_ok and glibc_ok else 1


if __name__ == "__main__":
    sys.exit(main())
