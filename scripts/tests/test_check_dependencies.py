#!/usr/bin/env python3
"""Behavioural tests for check-dependencies.py.

Two claims are checked on a built game: every shared library it names is one a
player already has, and nothing it asks of glibc is newer than the oldest glibc
it promises to run on. The second is where a naive checker goes wrong without
anyone noticing: glibc versions compared as strings put 2.9 above 2.31, and a
ceiling that silently passes is worse than none.

readelf is faked rather than run, so the cases are exact and the tests need no
compiled artifact to point at.
"""

import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "check-dependencies.py"
SKIP_EXIT_CODE = 77

SYSTEM_LIBRARIES = ("libc.so.6", "libm.so.6", "ld-linux-x86-64.so.2")

DYNAMIC_SECTION = """\
Dynamic section at offset 0x1234 contains 3 entries:
  Tag        Type                         Name/Value
 0x0000000000000001 (NEEDED)             Shared library: [libm.so.6]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
 0x0000000000000001 (NEEDED)             Shared library: [ld-linux-x86-64.so.2]
"""

DYNAMIC_SECTION_WITH_LIBSTDCXX = DYNAMIC_SECTION + """\
 0x0000000000000001 (NEEDED)             Shared library: [libstdc++.so.6]
"""


def version_needs(*needs: tuple[str, str]) -> str:
    """readelf -V output naming each (library, version) pair the binary needs."""
    lines = ["Version needs section '.gnu.version_r' contains 1 entries:",
             " Addr: 0x00000000000028f8  Offset: 0x000028f8  Link: 6 (.dynstr)"]
    for library, version in needs:
        lines.append(f"  000000: Version: 1  File: {library}  Cnt: 1")
        lines.append(f"  0x0010:   Name: {version}  Flags: none  Version: 2")
    return "\n".join(lines) + "\n"


class FakeReadelf:
    """A readelf that answers -d and -V from canned text.

    Written to a real file because the checker resolves the tool through
    shutil.which and then executes it, which a stubbed function would not cover.
    """

    def __init__(self, directory: Path, dynamic: str, versions: str = ""):
        self.path = directory / "fake-readelf"
        script = (
            "#!/bin/sh\n"
            'case "$1" in\n'
            f"  -d) cat <<'READELF_EOF'\n{dynamic}READELF_EOF\n  ;;\n"
            f"  -V) cat <<'READELF_EOF'\n{versions}READELF_EOF\n  ;;\n"
            "  *) exit 1 ;;\n"
            "esac\n"
        )
        self.path.write_text(script)
        self.path.chmod(self.path.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)


def run_checker(readelf, binary, allow=SYSTEM_LIBRARIES, max_glibc=None):
    command = [sys.executable, str(SCRIPT), "--binary", str(binary), "--readelf", str(readelf)]
    for name in allow:
        command += ["--allow", name]
    if max_glibc is not None:
        command += ["--max-glibc", max_glibc]
    return subprocess.run(command, capture_output=True, text=True, check=False)


class CheckDependenciesTest(unittest.TestCase):
    def setUp(self):
        self._directory = tempfile.TemporaryDirectory()
        self.directory = Path(self._directory.name)
        # The checker only hands the path to readelf, which is faked, but a
        # binary that is not there at all is a case of its own below.
        self.binary = self.directory / "game"
        self.binary.write_bytes(b"")

    def tearDown(self):
        self._directory.cleanup()

    def test_only_allowed_libraries_passes(self):
        readelf = FakeReadelf(self.directory, DYNAMIC_SECTION)
        result = run_checker(readelf.path, self.binary)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_library_outside_the_list_fails_and_is_named(self):
        readelf = FakeReadelf(self.directory, DYNAMIC_SECTION_WITH_LIBSTDCXX)
        result = run_checker(readelf.path, self.binary)
        self.assertEqual(result.returncode, 1)
        self.assertIn("libstdc++.so.6", result.stderr)

    def test_a_glibc_newer_than_the_ceiling_fails_and_is_named(self):
        readelf = FakeReadelf(self.directory, DYNAMIC_SECTION,
                              version_needs(("libc.so.6", "GLIBC_2.2.5"), ("libm.so.6", "GLIBC_2.43")))
        result = run_checker(readelf.path, self.binary, max_glibc="2.31")
        self.assertEqual(result.returncode, 1)
        self.assertIn("GLIBC_2.43", result.stderr)
        self.assertIn("libm.so.6", result.stderr)

    def test_versions_compare_as_numbers_not_strings(self):
        # As strings, "2.9" > "2.31" and "2.2.5" > "2.2", so a string compare
        # fails this binary for versions far below the ceiling.
        readelf = FakeReadelf(self.directory, DYNAMIC_SECTION,
                              version_needs(("libc.so.6", "GLIBC_2.9"), ("libc.so.6", "GLIBC_2.2.5"),
                                            ("libc.so.6", "GLIBC_2.31")))
        result = run_checker(readelf.path, self.binary, max_glibc="2.31")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_glibc_entries_without_a_version_number_are_not_versions(self):
        readelf = FakeReadelf(self.directory, DYNAMIC_SECTION,
                              version_needs(("libc.so.6", "GLIBC_2.30"), ("libc.so.6", "GLIBC_PRIVATE"),
                                            ("libc.so.6", "GLIBC_ABI_DT_RELR")))
        result = run_checker(readelf.path, self.binary, max_glibc="2.31")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_no_ceiling_means_no_glibc_check(self):
        # A host build is allowed to need its own glibc; the ceiling is only
        # passed for a build that promises an older one.
        readelf = FakeReadelf(self.directory, DYNAMIC_SECTION, version_needs(("libc.so.6", "GLIBC_2.43")))
        result = run_checker(readelf.path, self.binary)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_ceiling_with_no_version_needs_skips_rather_than_passing(self):
        # Every dynamically linked Linux program needs some glibc version, so an
        # empty list means readelf's output was not understood.
        readelf = FakeReadelf(self.directory, DYNAMIC_SECTION, versions="")
        result = run_checker(readelf.path, self.binary, max_glibc="2.31")
        self.assertEqual(result.returncode, SKIP_EXIT_CODE, result.stderr)

    def test_a_malformed_ceiling_is_refused(self):
        readelf = FakeReadelf(self.directory, DYNAMIC_SECTION, version_needs(("libc.so.6", "GLIBC_2.30")))
        result = run_checker(readelf.path, self.binary, max_glibc="two-point-thirty-one")
        self.assertNotEqual(result.returncode, 0)

    def test_a_missing_readelf_skips_rather_than_passing(self):
        result = run_checker("readelf-that-does-not-exist", self.binary)
        self.assertEqual(result.returncode, SKIP_EXIT_CODE, result.stderr)

    def test_a_missing_binary_skips_rather_than_passing(self):
        readelf = FakeReadelf(self.directory, DYNAMIC_SECTION)
        result = run_checker(readelf.path, self.directory / "not-built")
        self.assertEqual(result.returncode, SKIP_EXIT_CODE, result.stderr)


if __name__ == "__main__":
    unittest.main()
