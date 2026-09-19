#!/usr/bin/env python3
"""Behavioural tests for steamrt.py.

What matters is what the helper asks a container runtime to do: which runtime
it picks, that the multi-gigabyte SDK is pulled at its pinned digest only when
it is missing, that the build runs as the developer rather than as root, and
that a boot check with nothing to boot says so instead of passing.

podman and docker are faked by shell scripts that log their arguments, on a
PATH holding nothing else, so the real ones on this machine are never touched.
"""

import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "steamrt.py"
REPO = SCRIPT.parent.parent
SKIP_EXIT_CODE = 77


def write_fake_runtime(directory: Path, name: str) -> None:
    """A runtime that appends its command line to $FAKE_LOG.

    `image inspect` exits with $FAKE_INSPECT_CODE and `run` with
    $FAKE_RUN_CODE, both 0 by default, so a test decides whether an image is
    present and whether a container succeeds.
    """
    path = directory / name
    path.write_text(
        "#!/bin/sh\n"
        f'echo "{name} $*" >> "$FAKE_LOG"\n'
        'if [ "$1" = "image" ] && [ "$2" = "inspect" ]; then exit "${FAKE_INSPECT_CODE:-0}"; fi\n'
        'if [ "$1" = "run" ]; then exit "${FAKE_RUN_CODE:-0}"; fi\n'
        "exit 0\n")
    path.chmod(path.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)


class SteamRuntimeTest(unittest.TestCase):
    def setUp(self):
        self._directory = tempfile.TemporaryDirectory()
        self.directory = Path(self._directory.name)
        self.bin = self.directory / "bin"
        self.bin.mkdir()
        self.log = self.directory / "runtime.log"
        self.log.write_text("")

    def tearDown(self):
        self._directory.cleanup()

    def helper(self, *arguments: str, inspect_code: int = 0, run_code: int = 0):
        environment = {
            "PATH": str(self.bin),
            "FAKE_LOG": str(self.log),
            "FAKE_INSPECT_CODE": str(inspect_code),
            "FAKE_RUN_CODE": str(run_code),
        }
        return subprocess.run([sys.executable, str(SCRIPT), *arguments], capture_output=True, text=True,
                              check=False, env=environment, stdin=subprocess.DEVNULL)

    def calls(self) -> list[str]:
        return self.log.read_text().splitlines()

    def test_no_runtime_stops_and_names_both(self):
        result = self.helper("prepare")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("podman", result.stderr)
        self.assertIn("docker", result.stderr)

    def test_podman_is_preferred_over_docker(self):
        write_fake_runtime(self.bin, "podman")
        write_fake_runtime(self.bin, "docker")
        result = self.helper("prepare")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.calls())
        self.assertTrue(all(call.startswith("podman ") for call in self.calls()), self.calls())

    def test_docker_is_used_when_podman_is_absent(self):
        write_fake_runtime(self.bin, "docker")
        result = self.helper("prepare")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.calls())
        self.assertTrue(all(call.startswith("docker ") for call in self.calls()), self.calls())

    def test_a_missing_image_is_pulled_by_digest_and_the_derived_image_built(self):
        write_fake_runtime(self.bin, "podman")
        result = self.helper("prepare", inspect_code=1)
        self.assertEqual(result.returncode, 0, result.stderr)
        pulls = [call for call in self.calls() if call.startswith("podman pull ")]
        self.assertEqual(len(pulls), 1, self.calls())
        self.assertIn("@sha256:", pulls[0])
        self.assertTrue(any(call.startswith("podman build ") for call in self.calls()), self.calls())

    def test_images_already_present_are_neither_pulled_nor_built(self):
        write_fake_runtime(self.bin, "podman")
        result = self.helper("prepare", inspect_code=0)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(any(call.startswith(("podman pull ", "podman build ")) for call in self.calls()),
                         self.calls())

    def run_command(self) -> str:
        runs = [call for call in self.calls() if " run " in f" {call} "]
        self.assertEqual(len(runs), 1, self.calls())
        return runs[0]

    def test_podman_builds_as_the_developer_in_the_repository_at_its_own_path(self):
        write_fake_runtime(self.bin, "podman")
        result = self.helper("run", "--", "make", "gcc-ship-steamrt")
        self.assertEqual(result.returncode, 0, result.stderr)
        run = self.run_command()
        self.assertIn("--userns=keep-id", run)
        self.assertIn(f"-v {REPO}:{REPO}", run)
        self.assertTrue(run.endswith("make gcc-ship-steamrt"), run)

    def test_docker_builds_as_the_developer_in_the_repository_at_its_own_path(self):
        write_fake_runtime(self.bin, "docker")
        result = self.helper("run", "--", "make", "gcc-ship-steamrt")
        self.assertEqual(result.returncode, 0, result.stderr)
        run = self.run_command()
        self.assertIn(f"--user {os.getuid()}:{os.getgid()}", run)
        self.assertIn(f"-v {REPO}:{REPO}", run)

    def test_a_failed_build_reaches_the_exit_code(self):
        write_fake_runtime(self.bin, "podman")
        result = self.helper("run", "--", "make", "gcc-ship-steamrt", run_code=2)
        self.assertNotEqual(result.returncode, 0)

    def test_boot_check_with_nothing_staged_skips_rather_than_passing(self):
        write_fake_runtime(self.bin, "podman")
        result = self.helper("boot-check", "--staged", str(self.directory / "not-staged"))
        self.assertEqual(result.returncode, SKIP_EXIT_CODE, result.stderr)

    def test_boot_check_runs_the_staged_game_in_the_clean_system(self):
        write_fake_runtime(self.bin, "podman")
        staged = self.directory / "staged"
        staged.mkdir()
        (staged / "Assisi-Game").write_bytes(b"")
        result = self.helper("boot-check", "--staged", str(staged))
        self.assertEqual(result.returncode, 0, result.stderr)
        run = self.run_command()
        self.assertIn(f"{staged}:/game:ro", run)
        self.assertIn("--headless", run)

    def test_a_game_that_does_not_boot_fails_the_boot_check(self):
        write_fake_runtime(self.bin, "podman")
        staged = self.directory / "staged"
        staged.mkdir()
        (staged / "Assisi-Game").write_bytes(b"")
        result = self.helper("boot-check", "--staged", str(staged), run_code=1)
        self.assertEqual(result.returncode, 1, result.stderr)


if __name__ == "__main__":
    unittest.main()
