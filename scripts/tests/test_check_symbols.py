#!/usr/bin/env python3
"""Behavioural tests for check-symbols.py.

The scanner is what stands between a shipped game and a relinked editor, so the
case that matters most here is not "it finds a forbidden symbol" -- it is that a
scan which could not run says so instead of reporting success. A stripped binary
yields an empty symbol table, and a scanner that called that clean would pass
hardest on exactly the build nobody can inspect by hand.

nm is faked rather than run, so the cases are exact and the tests need no
compiled artifact to point at.
"""

import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "check-symbols.py"
SKIP_EXIT_CODE = 77

CLEAN_OUTPUT = """\
0000000000001234 T Assisi::App::Application::Run()
0000000000005678 T Assisi::Render::MeshPass::Draw()
000000000000abcd T main
"""

DIRTY_OUTPUT = CLEAN_OUTPUT + """\
000000000000dead T ImGui::Begin(char const*, bool*, int)
000000000000beef T Assisi::Editor::EditorApp::OnStart()
"""


class FakeNm:
    """An `nm` that prints what the test tells it to, with the exit code it says.

    Written to a real file because the scanner resolves the tool through
    shutil.which and then executes it, which a stubbed function would not cover.
    """

    def __init__(self, directory: Path, stdout: str = "", stderr: str = "", code: int = 0):
        self.path = directory / "fake-nm"
        script = (
            "#!/bin/sh\n"
            f"cat <<'NM_EOF'\n{stdout}NM_EOF\n"
            f"printf '%s' '{stderr}' >&2\n"
            f"exit {code}\n"
        )
        self.path.write_text(script)
        self.path.chmod(self.path.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)


def run_scanner(nm_path, binary="some-binary", forbid=("ImGui::", "Assisi::Editor::")):
    command = [sys.executable, str(SCRIPT), "--binary", binary, "--nm", str(nm_path)]
    for pattern in forbid:
        command += ["--forbid", pattern]
    return subprocess.run(command, capture_output=True, text=True, check=False)


class CheckSymbolsTest(unittest.TestCase):
    def test_a_forbidden_symbol_fails_and_is_named(self):
        with tempfile.TemporaryDirectory() as directory:
            nm = FakeNm(Path(directory), stdout=DIRTY_OUTPUT)
            result = run_scanner(nm.path)
        self.assertEqual(result.returncode, 1)
        # The pattern and an example both appear, so the failure says which
        # library came back rather than only that something did.
        self.assertIn("ImGui::", result.stderr)
        self.assertIn("ImGui::Begin", result.stderr)
        self.assertIn("Assisi::Editor::EditorApp::OnStart", result.stderr)

    def test_a_clean_table_passes(self):
        with tempfile.TemporaryDirectory() as directory:
            nm = FakeNm(Path(directory), stdout=CLEAN_OUTPUT)
            result = run_scanner(nm.path)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_an_empty_table_skips_rather_than_passing(self):
        # The stripped-binary case. Reporting 0 here is the failure mode this
        # whole test file exists for.
        with tempfile.TemporaryDirectory() as directory:
            nm = FakeNm(Path(directory), stdout="")
            result = run_scanner(nm.path)
        self.assertEqual(result.returncode, SKIP_EXIT_CODE, result.stderr)
        self.assertIn("Nothing was checked", result.stderr)

    def test_nm_reporting_no_symbols_skips_rather_than_passing(self):
        # How nm actually reports a stripped binary: a non-zero exit with this
        # on stderr, not empty output.
        with tempfile.TemporaryDirectory() as directory:
            nm = FakeNm(Path(directory), stderr="fake-nm: some-binary: no symbols", code=1)
            result = run_scanner(nm.path)
        self.assertEqual(result.returncode, SKIP_EXIT_CODE, result.stderr)

    def test_a_missing_nm_skips_rather_than_passing(self):
        result = run_scanner("nm-that-does-not-exist")
        self.assertEqual(result.returncode, SKIP_EXIT_CODE, result.stderr)

    def test_no_patterns_proves_nothing_and_says_so(self):
        # A registration that lost its --forbid arguments would otherwise be a
        # test that passes against any binary at all.
        with tempfile.TemporaryDirectory() as directory:
            nm = FakeNm(Path(directory), stdout=DIRTY_OUTPUT)
            result = run_scanner(nm.path, forbid=())
        self.assertEqual(result.returncode, SKIP_EXIT_CODE, result.stderr)


if __name__ == "__main__":
    unittest.main()
