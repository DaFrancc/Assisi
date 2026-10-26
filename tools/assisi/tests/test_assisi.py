#!/usr/bin/env python3
"""Behavioural tests for the assisi build tool's command line.

What matters is what the tool asks the system to do: which cmake, ctest and
cook/pack invocations it makes, in what order, with which tier and codec, and
that it stops at the first failure. Those are the Makefile's results, and a
build that silently cooks at the wrong tier or packs after a failed build is
the regression this guards.

Every external program is a shell script on a PATH holding nothing else, which
appends its own command line to a log. The tool is driven in-process through
cli.main against a scratch repository, so nothing on this machine is built,
cleaned or formatted.
"""

from __future__ import annotations

import ast
import contextlib
import io
import json
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[2]
REPO = TOOLS.parent
sys.path.insert(0, str(TOOLS))

from assisi import cli, install, locate, project, request, store  # noqa: E402
from assisi.commands import catalog  # noqa: E402
from assisi.commands import test as test_command  # noqa: E402

# A fake program: logs "<name> <args>" and answers the few queries the tool
# makes. FAKE_FAIL_ON makes any call whose arguments contain it exit non-zero.
# A file named after -F is a list of files to process; each one is logged as
# "listed <file>", since the tool deletes the list once the call returns.
FAKE_PROGRAM = """#!/bin/sh
if [ $# -gt 0 ]; then echo "{name} $*" >> "$FAKE_LOG"; else echo "{name}" >> "$FAKE_LOG"; fi
previous=""
for argument in "$@"; do
  if [ "$previous" = "-F" ]; then
    while IFS= read -r line; do echo "listed $line" >> "$FAKE_LOG"; done < "$argument"
  fi
  previous="$argument"
done
if [ -n "$FAKE_FAIL_ON" ]; then
  case "{name} $*" in *"$FAKE_FAIL_ON"*) exit 3;; esac
fi
case "{name} $1" in
  "cmake --version") echo "cmake version ${{FAKE_CMAKE_VERSION:-3.31.8}}";;
  "ninja --version") echo "1.13.2";;
  "git ls-files") printf 'modules/Core/src/A.cpp\\nmodules/Core/include/A.hpp\\ntools/reflectgen/tests/golden/G.hpp\\ntools/reflectgen/tests/fixtures/F.hpp\\n';;
  "g++ -print-file-name=libstdc++.a") echo "${{FAKE_LIBSTDCXX:-/usr/lib/gcc/libstdc++.a}}";;
esac
exit 0
"""

FAKE_STEAMRT = """import sys, os
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write("steamrt " + " ".join(sys.argv[1:]) + "\\n")
sys.exit(int(os.environ.get("FAKE_STEAMRT_CODE", "0")))
"""

PROGRAMS = ("cmake", "ctest", "git", "uncrustify", "g++", "ninja", "ccache")


def make_executable(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)


class Sandbox:
    """A scratch repository, a PATH of fakes, and a log of what ran."""

    def __init__(self, directory: Path, programs: tuple[str, ...] = PROGRAMS):
        self.root = directory / "repo"
        self.bin = directory / "bin"
        self.log = directory / "calls.log"
        self.root.mkdir()
        self.bin.mkdir()
        self.log.write_text("")
        (self.root / "assets").mkdir()
        (self.root / "cmake").mkdir()
        (self.root / "cmake" / "AssisiConfigureCached.cmake").write_text("")
        (self.root / "scripts").mkdir()
        (self.root / "scripts" / "steamrt.py").write_text(FAKE_STEAMRT)
        for name in programs:
            make_executable(self.bin / name, FAKE_PROGRAM.format(name=name))

    def build_dir(self, preset: str) -> Path:
        return self.root / "out" / "build" / preset

    def configured(self, *presets: str) -> None:
        for preset in presets:
            self.build_dir(preset).mkdir(parents=True, exist_ok=True)
            (self.build_dir(preset) / "CMakeCache.txt").write_text("")

    def tools_built(self, preset: str) -> None:
        """The executables a build would have produced, as logging fakes."""
        apps = self.build_dir(preset) / "apps"
        for relative in ("cook/assisi-cook", "pack/assisi-pack", "game/Assisi-Game", "game/Assisi-GameEditor"):
            name = Path(relative).name
            make_executable(apps / relative, FAKE_PROGRAM.format(name=name))

    def run(self, *argv: str, interactive: bool = False, **environment: str):
        env = {"PATH": str(self.bin), "FAKE_LOG": str(self.log)}
        env.update(environment)
        out, err = io.StringIO(), io.StringIO()
        saved = dict(os.environ)
        try:
            os.environ.clear()
            os.environ.update(env)
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = cli.main(list(argv), root=self.root, host="Linux", interactive=interactive)
        finally:
            os.environ.clear()
            os.environ.update(saved)
        return code, out.getvalue(), err.getvalue()

    def calls(self) -> list[str]:
        return self.log.read_text().splitlines()

    def path(self, *parts: str) -> str:
        return str(self.root.joinpath(*parts))

    def configure_call(self, presets: str) -> str:
        return f"cmake -DPRESETS={presets} -P {self.path('cmake', 'AssisiConfigureCached.cmake')}"


class ToolTest(unittest.TestCase):
    def setUp(self):
        self._directory = tempfile.TemporaryDirectory()
        self.sandbox = Sandbox(Path(self._directory.name))

    def tearDown(self):
        self._directory.cleanup()


class PackageTest(ToolTest):
    def expected_package(self, preset: str, tier: str, codec: str) -> list[str]:
        build = self.sandbox.build_dir(preset)
        return [
            f"cmake --build --preset {preset}",
            f"cmake --build --preset {preset} --target Assisi-Game",
            f"cmake --build --preset {preset} --target Assisi-Cook-Tool Assisi-VkShaders",
            f"assisi-cook --source {self.sandbox.path('assets')} --out {build / 'cooked'} --texture-tier {tier}",
            f"cmake --build --preset {preset} --target Assisi-Pack-Tool",
            f"assisi-pack --cooked {build / 'cooked'} --out {build / 'apps' / 'game' / 'assets.pak'} "
            f"--compress {codec}",
        ]

    def test_each_level_cooks_and_packs_with_its_own_tier_and_codec(self):
        for level, tier, codec in (("debug", "fast", "none"), ("dev", "best", "lz4"), ("ship", "best", "zstd")):
            with self.subTest(level=level):
                preset = f"gcc-{level}"
                self.sandbox.log.write_text("")
                self.sandbox.configured(preset)
                self.sandbox.tools_built(preset)
                code, _, err = self.sandbox.run("package", level)
                self.assertEqual(code, 0, err)
                self.assertEqual(self.sandbox.calls(), self.expected_package(preset, tier, codec))

    def test_a_failed_step_stops_everything_after_it(self):
        self.sandbox.configured("gcc-ship")
        self.sandbox.tools_built("gcc-ship")
        code, _, _ = self.sandbox.run("package", "ship", FAKE_FAIL_ON="--target Assisi-Game")
        self.assertNotEqual(code, 0)
        self.assertEqual(self.sandbox.calls(), self.expected_package("gcc-ship", "best", "zstd")[:2])

    def test_package_needs_a_named_level(self):
        code, _, _ = self.sandbox.run("package")
        self.assertNotEqual(code, 0)
        self.assertEqual(self.sandbox.calls(), [])

    def test_the_profiler_build_takes_its_level_tier(self):
        self.sandbox.configured("gcc-ship-chiara")
        self.sandbox.tools_built("gcc-ship-chiara")
        code, _, err = self.sandbox.run("package", "ship", "--profiler")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.sandbox.calls(), self.expected_package("gcc-ship-chiara", "best", "zstd"))

    def test_another_compiler_packages_its_own_tree(self):
        self.sandbox.configured("clang-dev")
        self.sandbox.tools_built("clang-dev")
        code, _, err = self.sandbox.run("package", "dev", "--compiler", "clang")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.sandbox.calls(), self.expected_package("clang-dev", "best", "lz4"))


class ReleaseTest(ToolTest):
    """A release keeps what players got, and later packages lay out against it."""

    def setUp(self):
        super().setUp()
        for preset in ("gcc-ship", "gcc-dev"):
            self.sandbox.configured(preset)
            self.sandbox.tools_built(preset)
            pak = self.sandbox.build_dir(preset) / "apps" / "game" / "assets.pak"
            pak.write_text(f"pak of {preset}\n")

    def pack_call(self) -> str:
        return next(line for line in self.sandbox.calls() if line.startswith("assisi-pack"))

    def release_dir(self, version: str) -> Path:
        return self.sandbox.root / "releases" / version

    def test_without_a_release_the_pak_is_laid_out_fresh(self):
        self.assertEqual(self.sandbox.run("package", "ship")[0], 0)
        self.assertNotIn("--previous", self.pack_call())

    def test_a_release_keeps_the_game_and_its_pak(self):
        code, _, err = self.sandbox.run("release", "ship", "--version", "1.0")
        self.assertEqual(code, 0, err)
        self.assertIn("assisi-pack", " ".join(self.sandbox.calls()))
        kept = self.release_dir("1.0")
        self.assertEqual((kept / "assets.pak").read_text(), "pak of gcc-ship\n")
        self.assertTrue((kept / "Assisi-Game").exists())
        record = json.loads((kept / "release.json").read_text())
        self.assertEqual(record["level"], "ship")

    def test_a_steam_game_is_kept_with_its_library_and_never_its_app_id_file(self):
        game_dir = self.sandbox.build_dir("gcc-ship") / "apps" / "game"
        (game_dir / "libsteam_api.so").write_text("steam\n")
        (game_dir / "steam_appid.txt").write_text("480\n")
        self.assertEqual(self.sandbox.run("release", "ship", "--version", "1.0")[0], 0)
        kept = self.release_dir("1.0")
        self.assertTrue((kept / "libsteam_api.so").exists())
        self.assertFalse((kept / "steam_appid.txt").exists())

    def test_a_linux_game_is_kept_with_the_handler_that_writes_its_crash_dumps(self):
        game_dir = self.sandbox.build_dir("gcc-ship") / "apps" / "game"
        (game_dir / "crashpad_handler").write_text("handler\n")
        self.assertEqual(self.sandbox.run("release", "ship", "--version", "1.0")[0], 0)
        self.assertTrue((self.release_dir("1.0") / "crashpad_handler").exists())

    def test_later_packages_lay_out_against_the_newest_release_of_their_level(self):
        self.assertEqual(self.sandbox.run("release", "ship", "--version", "1.0")[0], 0)
        self.assertEqual(self.sandbox.run("release", "ship", "--version", "1.1")[0], 0)
        self.sandbox.log.write_text("")
        code, _, err = self.sandbox.run("package", "ship")
        self.assertEqual(code, 0, err)
        self.assertIn(f"--previous {self.release_dir('1.1') / 'assets.pak'}", self.pack_call())

    def test_a_release_of_another_level_is_not_used(self):
        self.assertEqual(self.sandbox.run("release", "ship", "--version", "1.0")[0], 0)
        self.sandbox.log.write_text("")
        self.assertEqual(self.sandbox.run("package", "dev")[0], 0)
        self.assertNotIn("--previous", self.pack_call())

    def test_fresh_ignores_the_releases(self):
        self.assertEqual(self.sandbox.run("release", "ship", "--version", "1.0")[0], 0)
        self.sandbox.log.write_text("")
        self.assertEqual(self.sandbox.run("package", "ship", "--fresh")[0], 0)
        self.assertNotIn("--previous", self.pack_call())

    def test_a_named_previous_pak_is_used_as_given(self):
        named = self.sandbox.root / "old.pak"
        named.write_text("old\n")
        self.assertEqual(self.sandbox.run("release", "ship", "--version", "1.0")[0], 0)
        self.sandbox.log.write_text("")
        self.assertEqual(self.sandbox.run("pack", "ship", "--previous", "old.pak")[0], 0)
        self.assertIn(f"--previous {named}", self.pack_call())

    def test_a_missing_previous_pak_is_refused_before_anything_runs(self):
        code, _, _ = self.sandbox.run("package", "ship", "--previous", "nowhere.pak")
        self.assertNotEqual(code, 0)
        self.assertEqual(self.sandbox.calls(), [])

    def test_previous_and_fresh_together_are_refused(self):
        (self.sandbox.root / "old.pak").write_text("old\n")
        code, _, _ = self.sandbox.run("package", "ship", "--previous", "old.pak", "--fresh")
        self.assertNotEqual(code, 0)
        self.assertEqual(self.sandbox.calls(), [])

    def test_a_version_is_never_overwritten(self):
        self.assertEqual(self.sandbox.run("release", "ship", "--version", "1.0")[0], 0)
        self.sandbox.log.write_text("")
        code, _, _ = self.sandbox.run("release", "ship", "--version", "1.0")
        self.assertNotEqual(code, 0)
        self.assertEqual(self.sandbox.calls(), [])

    def test_a_release_needs_a_version(self):
        code, _, _ = self.sandbox.run("release", "ship")
        self.assertNotEqual(code, 0)
        self.assertEqual(self.sandbox.calls(), [])

    def test_a_failed_package_keeps_nothing(self):
        code, _, _ = self.sandbox.run("release", "ship", "--version", "1.0", FAKE_FAIL_ON="assisi-pack")
        self.assertNotEqual(code, 0)
        self.assertFalse(self.release_dir("1.0").exists())

    def test_a_steam_runtime_release_packages_in_the_container_and_keeps_on_the_host(self):
        self.sandbox.configured("gcc-ship-steamrt")
        self.sandbox.tools_built("gcc-ship-steamrt")
        (self.sandbox.build_dir("gcc-ship-steamrt") / "apps" / "game" / "assets.pak").write_text("steamrt pak\n")
        code, _, err = self.sandbox.run("release", "ship", "--steam-runtime", "--version", "1.0")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.sandbox.calls(), [
            f"steamrt run -- python3 {self.sandbox.path('assisi')} package ship --compiler gcc --steam-runtime"])
        self.assertEqual((self.release_dir("1.0") / "assets.pak").read_text(), "steamrt pak\n")


class BuildTest(ToolTest):
    def test_unconfigured_build_is_configured_first(self):
        code, _, err = self.sandbox.run("build", "dev")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.sandbox.calls(), [self.sandbox.configure_call("gcc-dev"),
                                                "cmake --build --preset gcc-dev"])

    def test_configured_build_is_not_configured_again(self):
        self.sandbox.configured("gcc-dev")
        self.assertEqual(self.sandbox.run("build", "dev")[0], 0)
        self.assertEqual(self.sandbox.calls(), ["cmake --build --preset gcc-dev"])

    def test_several_levels_configure_together_then_build_each(self):
        code, _, err = self.sandbox.run("build", "debug", "dev", "ship")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.sandbox.calls(), [
            self.sandbox.configure_call("gcc-debug;gcc-dev;gcc-ship"),
            "cmake --build --preset gcc-debug",
            "cmake --build --preset gcc-dev",
            "cmake --build --preset gcc-ship",
        ])

    def test_game_flag_builds_the_game_after_the_default_target(self):
        self.sandbox.configured("gcc-dev")
        self.assertEqual(self.sandbox.run("build", "dev", "--game")[0], 0)
        self.assertEqual(self.sandbox.calls(), [
            "cmake --build --preset gcc-dev",
            "cmake --build --preset gcc-dev --target Assisi-Game",
        ])

    def test_configure_always_runs_the_cached_script(self):
        self.sandbox.configured("clang-dev")
        self.assertEqual(self.sandbox.run("configure", "dev", "--compiler", "clang")[0], 0)
        self.assertEqual(self.sandbox.calls(), [self.sandbox.configure_call("clang-dev")])

    def test_sanitizer_builds(self):
        for argv, preset in ((["debug", "--sanitize", "address"], "gcc-asan"),
                             (["debug", "--sanitize", "thread"], "gcc-tsan"),
                             (["debug", "--sanitize", "thread", "--profiler"], "gcc-tsan-chiara"),
                             (["debug", "--sanitize", "address", "--compiler", "clang"], "clang-asan")):
            with self.subTest(argv=argv):
                self.sandbox.log.write_text("")
                code, _, err = self.sandbox.run("build", *argv)
                self.assertEqual(code, 0, err)
                self.assertEqual(self.sandbox.calls()[-1], f"cmake --build --preset {preset}")


class NameTest(ToolTest):
    def tree(self, spec: project.BuildSpec, host: str = "Linux") -> str:
        return project.Project(self.sandbox.root, host=host).tree_for(spec)

    def test_level_and_compiler_name_the_preset(self):
        self.assertEqual(self.tree(project.BuildSpec("gcc", "dev")), "gcc-dev")
        self.assertEqual(self.tree(project.BuildSpec("clang", "ship", profiler=True)), "clang-ship-chiara")
        self.assertEqual(self.tree(project.BuildSpec("gcc", "ship", steam_runtime=True)), "gcc-ship-steamrt")
        self.assertEqual(self.tree(project.BuildSpec("msvc", "debug", sanitize="address"), host="Windows"),
                         "msvc-asan")

    def test_every_preset_reads_back_as_the_spec_that_names_it(self):
        windows = project.Project(self.sandbox.root, host="Windows")
        linux = project.Project(self.sandbox.root, host="Linux")
        for tree in project.all_trees():
            with self.subTest(tree=tree):
                host = windows if project.compiler_of(tree) == "msvc" else linux
                self.assertEqual(host.tree_for(project.spec_of(tree)), tree)

    def test_combinations_with_no_preset_are_refused(self):
        refused = (project.BuildSpec("gcc", "dev", sanitize="address"),
                   project.BuildSpec("gcc", "debug", sanitize="address", profiler=True),
                   project.BuildSpec("clang", "ship", steam_runtime=True),
                   project.BuildSpec("gcc", "dev", steam_runtime=True),
                   project.BuildSpec("gcc", "ship", profiler=True, steam_runtime=True))
        for spec in refused:
            with self.subTest(spec=spec):
                with self.assertRaises(project.UsageError):
                    self.tree(spec)
        with self.assertRaises(project.UsageError):
            self.tree(project.BuildSpec("msvc", "debug", sanitize="thread"), host="Windows")

    def test_another_hosts_compiler_is_refused(self):
        code, _, _ = self.sandbox.run("build", "dev", "--compiler", "msvc")
        self.assertNotEqual(code, 0)
        self.assertEqual(self.sandbox.calls(), [])

    def test_unknown_level_is_refused(self):
        code, _, _ = self.sandbox.run("build", "fast")
        self.assertNotEqual(code, 0)
        self.assertEqual(self.sandbox.calls(), [])

    def test_saved_defaults_are_used_when_nothing_is_named(self):
        self.sandbox.configured("clang-ship")
        self.assertEqual(self.sandbox.run("default", "ship", "--compiler", "clang")[0], 0)
        self.assertEqual(self.sandbox.run("build")[0], 0)
        self.assertEqual(self.sandbox.calls(), ["cmake --build --preset clang-ship"])

    def test_without_saved_defaults_the_host_dev_build_is_used(self):
        self.sandbox.configured("gcc-dev")
        self.assertEqual(self.sandbox.run("build")[0], 0)
        self.assertEqual(self.sandbox.calls(), ["cmake --build --preset gcc-dev"])

    def test_every_build_the_tool_knows_is_a_preset_and_back(self):
        # Against the repository's real CMakePresets.json: a preset added there
        # and not here, or named here and not there, fails this.
        presets = json.loads((REPO / "CMakePresets.json").read_text(encoding="utf-8"))
        configure = {entry["name"] for entry in presets["configurePresets"] if not entry.get("hidden")}
        build = {entry["name"] for entry in presets["buildPresets"]}
        test = {entry["name"] for entry in presets["testPresets"]}
        known = set(project.all_trees())
        self.assertEqual(known, configure)
        self.assertTrue(known <= build, known - build)
        self.assertTrue(known <= test, known - test)


class SteamRuntimeTest(ToolTest):
    def test_on_the_host_the_build_runs_itself_inside_the_container(self):
        code, _, err = self.sandbox.run("build", "ship", "--steam-runtime")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.sandbox.calls(), [
            f"steamrt run -- python3 {self.sandbox.path('assisi')} build ship --compiler gcc --steam-runtime"])

    def test_on_the_host_test_also_boots_the_staged_game(self):
        self.assertEqual(self.sandbox.run("test", "ship", "--steam-runtime")[0], 0)
        self.assertEqual(self.sandbox.calls(), [
            f"steamrt run -- python3 {self.sandbox.path('assisi')} test ship --compiler gcc --steam-runtime",
            "steamrt boot-check",
        ])

    def test_a_failed_container_run_skips_the_boot_check(self):
        code, _, _ = self.sandbox.run("test", "ship", "--steam-runtime", FAKE_STEAMRT_CODE="2")
        self.assertNotEqual(code, 0)
        self.assertEqual(len(self.sandbox.calls()), 1)

    def test_inside_the_container_it_is_an_ordinary_build_with_the_game_tests(self):
        code, _, err = self.sandbox.run("test", "ship", "--steam-runtime", ASSISI_STEAMRT_SDK="1")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.sandbox.calls(), [
            self.sandbox.configure_call("gcc-ship-steamrt"),
            "cmake --build --preset gcc-ship-steamrt",
            f"ctest --preset gcc-ship-steamrt -j{test_command.parallel_jobs()} -L game|container",
        ])

    def test_fetch_and_remove(self):
        stale = self.sandbox.root / "out" / "steamrt-home"
        stale.mkdir(parents=True)
        self.assertEqual(self.sandbox.run("steamrt", "fetch")[0], 0)
        self.assertEqual(self.sandbox.run("steamrt", "remove")[0], 0)
        self.assertEqual(self.sandbox.calls(), ["steamrt prepare", "steamrt remove"])
        self.assertFalse(stale.exists())


class TestCommandTest(ToolTest):
    def setUp(self):
        super().setUp()
        jobs = mock.patch("assisi.commands.test.parallel_jobs", return_value=7)
        jobs.start()
        self.addCleanup(jobs.stop)

    def test_builds_then_runs_ctest_with_passthrough(self):
        self.sandbox.configured("gcc-debug")
        code, _, err = self.sandbox.run("test", "debug", "--", "-R", "Audio")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.sandbox.calls(), ["cmake --build --preset gcc-debug",
                                                "ctest --preset gcc-debug -j7 -R Audio"])

    def test_ctest_runs_a_test_per_cpu_at_once(self):
        self.sandbox.configured("gcc-debug")
        code, _, err = self.sandbox.run("test", "debug")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.sandbox.calls()[-1], "ctest --preset gcc-debug -j7")

    def test_a_jobs_count_passed_through_is_the_only_one(self):
        self.sandbox.configured("gcc-debug")
        for jobs in (["-j2"], ["-j", "2"], ["--parallel", "2"], ["--parallel=2"], ["-j"]):
            with self.subTest(jobs=jobs):
                self.sandbox.log.write_text("")
                code, _, err = self.sandbox.run("test", "debug", "--", *jobs)
                self.assertEqual(code, 0, err)
                self.assertEqual(self.sandbox.calls()[-1], " ".join(["ctest --preset gcc-debug", *jobs]))


class CleanTest(ToolTest):
    def test_named_levels_take_only_their_own_trees(self):
        self.sandbox.configured("gcc-debug", "gcc-dev", "gcc-ship", "clang-dev")
        self.assertEqual(self.sandbox.run("clean", "debug", "dev")[0], 0)
        left = sorted(path.name for path in (self.sandbox.root / "out" / "build").iterdir())
        self.assertEqual(left, ["clang-dev", "gcc-ship"])

    def test_deps_removes_only_the_dependency_cache(self):
        self.sandbox.configured("gcc-dev")
        deps = self.sandbox.root / "out" / "_deps-src"
        deps.mkdir(parents=True)
        self.assertEqual(self.sandbox.run("clean", "--deps")[0], 0)
        self.assertFalse(deps.exists())
        self.assertTrue(self.sandbox.build_dir("gcc-dev").exists())

    def test_all_removes_every_build_and_keeps_the_dependency_cache(self):
        self.sandbox.configured("gcc-dev", "clang-ship", "gcc-tsan-chiara", "gcc-ship-steamrt")
        deps = self.sandbox.root / "out" / "_deps-src"
        deps.mkdir(parents=True)
        self.assertEqual(self.sandbox.run("clean", "--all")[0], 0)
        self.assertEqual(list((self.sandbox.root / "out" / "build").iterdir()), [])
        self.assertTrue(deps.exists())

    def test_clean_needs_something_named(self):
        self.sandbox.configured("gcc-dev")
        code, _, _ = self.sandbox.run("clean")
        self.assertNotEqual(code, 0)
        self.assertTrue(self.sandbox.build_dir("gcc-dev").exists())

    def test_levels_and_all_together_are_refused(self):
        self.sandbox.configured("gcc-dev")
        self.assertNotEqual(self.sandbox.run("clean", "dev", "--all")[0], 0)
        self.assertTrue(self.sandbox.build_dir("gcc-dev").exists())


class FormatTest(ToolTest):
    def test_format_runs_twice_without_backups_over_the_tracked_sources(self):
        code, _, err = self.sandbox.run("format")
        self.assertEqual(code, 0, err)
        runs = [line for line in self.sandbox.calls() if line.startswith("uncrustify")]
        self.assertEqual(len(runs), 2)
        for run in runs:
            self.assertIn("--no-backup", run.split())
            self.assertIn("-F", run.split())
        listed = [line for line in self.sandbox.calls() if line.startswith("listed ")]
        self.assertEqual(listed, ["listed modules/Core/src/A.cpp", "listed modules/Core/include/A.hpp"] * 2)

    def test_check_runs_once(self):
        self.assertEqual(self.sandbox.run("format", "--check")[0], 0)
        runs = [line for line in self.sandbox.calls() if line.startswith("uncrustify")]
        self.assertEqual(len(runs), 1)
        self.assertIn("--check", runs[0].split())


class DoctorTest(unittest.TestCase):
    def setUp(self):
        self._directory = tempfile.TemporaryDirectory()

    def tearDown(self):
        self._directory.cleanup()

    def sandbox(self, programs: tuple[str, ...]) -> Sandbox:
        return Sandbox(Path(self._directory.name), programs)

    def test_missing_tools_fail(self):
        box = self.sandbox(())
        self.assertNotEqual(box.run("doctor")[0], 0)

    def test_everything_present_passes(self):
        box = self.sandbox(PROGRAMS)
        code, out, _ = box.run("doctor")
        self.assertEqual(code, 0, out)

    def test_old_cmake_fails(self):
        box = self.sandbox(PROGRAMS)
        self.assertNotEqual(box.run("doctor", FAKE_CMAKE_VERSION="3.20.0")[0], 0)

    def test_missing_static_libstdcxx_fails(self):
        box = self.sandbox(PROGRAMS)
        self.assertNotEqual(box.run("doctor", FAKE_LIBSTDCXX="libstdc++.a")[0], 0)


class RunTest(ToolTest):
    def test_editor_runs_by_absolute_path_with_its_arguments(self):
        self.sandbox.tools_built("gcc-dev")
        code, _, err = self.sandbox.run("run", "editor", "dev", "--no-build", "--", "-l", "x.alvl")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.sandbox.calls(), ["Assisi-GameEditor -l x.alvl"])

    def test_run_builds_first_unless_told_not_to(self):
        self.sandbox.configured("gcc-dev")
        self.sandbox.tools_built("gcc-dev")
        self.assertEqual(self.sandbox.run("run", "editor")[0], 0)
        self.assertEqual(self.sandbox.calls(), ["cmake --build --preset gcc-dev", "Assisi-GameEditor"])

    def test_the_editor_runs_under_a_sanitizer_build(self):
        self.sandbox.tools_built("gcc-asan")
        code, _, err = self.sandbox.run("run", "editor", "debug", "--sanitize", "address", "--no-build")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.sandbox.calls(), ["Assisi-GameEditor"])

    def test_the_exit_code_of_the_program_is_passed_through(self):
        self.sandbox.tools_built("gcc-dev")
        code, _, _ = self.sandbox.run("run", "editor", "dev", "--no-build", FAKE_FAIL_ON="Assisi-GameEditor")
        self.assertEqual(code, 3)

    def test_game_without_a_package_runs_nothing(self):
        self.sandbox.tools_built("gcc-dev")
        code, _, _ = self.sandbox.run("run", "game", "dev", "--no-build")
        self.assertNotEqual(code, 0)
        self.assertEqual(self.sandbox.calls(), [])


class RequestTest(ToolTest):
    EXAMPLES = (
        ["build", "dev", "--game"],
        ["build", "debug", "dev", "ship", "--compiler", "clang"],
        ["build", "debug", "--sanitize", "thread", "--profiler"],
        ["package", "ship"],
        ["package", "ship", "--steam-runtime"],
        ["package", "ship", "--fresh"],
        ["release", "ship", "--version", "1.2"],
        ["cook", "debug"],
        ["test", "debug", "--", "-R", "Audio"],
        ["run", "editor", "dev", "--no-build", "--", "-l", "x.alvl"],
        ["clean", "debug", "dev"],
        ["clean", "--all", "--deps"],
        ["format", "--check"],
        ["steamrt", "fetch"],
    )

    def test_every_request_survives_its_own_command_line(self):
        tool = project.Project(self.sandbox.root, host="Linux")
        for argv in self.EXAMPLES:
            with self.subTest(argv=argv):
                first = request.parse(argv, tool)
                again = request.parse(first.to_argv(), tool)
                self.assertEqual(first, again)

    def test_defaults_are_written_out_in_full(self):
        tool = project.Project(self.sandbox.root, host="Linux")
        self.assertEqual(request.parse(["build"], tool).to_argv(), ["build", "dev", "--compiler", "gcc"])
        self.assertEqual(request.parse(["run", "editor"], tool).to_argv(),
                         ["run", "editor", "dev", "--compiler", "gcc"])

    def test_every_command_declares_the_interface(self):
        names = [command.NAME for command in catalog()]
        self.assertEqual(len(names), len(set(names)))
        for command in catalog():
            for attribute in ("NAME", "HELP", "OPTIONS", "RECORDED", "execute"):
                self.assertTrue(hasattr(command, attribute), f"{command.__name__} has no {attribute}")

    def test_steps_that_make_a_game_take_no_sanitizer(self):
        tool = project.Project(self.sandbox.root, host="Linux")
        for argv in (["package", "debug", "--sanitize", "address"], ["cook", "debug", "--sanitize", "thread"],
                     ["run", "game", "debug", "--sanitize", "address"]):
            with self.subTest(argv=argv):
                with self.assertRaises(project.UsageError):
                    request.parse(argv, tool)


class HistoryTest(ToolTest):
    CANONICAL = ["build", "dev", "--compiler", "gcc"]

    def test_repeated_runs_count_up_and_failures_are_kept(self):
        self.sandbox.configured("gcc-dev")
        self.sandbox.run("build", "dev")
        self.sandbox.run("build", "dev")
        self.sandbox.run("build", "dev", FAKE_FAIL_ON="--build")
        entries = store.Store(self.sandbox.root).history()
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0].argv, self.CANONICAL)
        self.assertEqual(entries[0].count, 3)
        self.assertFalse(entries[0].last_ok)

    def test_spellings_of_one_build_share_one_entry(self):
        self.sandbox.configured("gcc-dev")
        self.sandbox.run("build")
        self.sandbox.run("build", "dev")
        self.sandbox.run("build", "dev", "--compiler", "gcc")
        self.assertEqual(len(store.Store(self.sandbox.root).history()), 1)

    def test_again_replays_the_last_request(self):
        self.sandbox.configured("gcc-dev", "gcc-ship")
        self.sandbox.run("build", "ship")
        self.sandbox.run("build", "dev")
        self.sandbox.log.write_text("")
        self.assertEqual(self.sandbox.run("again")[0], 0)
        self.assertEqual(self.sandbox.run("again", "2")[0], 0)
        self.assertEqual(self.sandbox.calls(), ["cmake --build --preset gcc-dev", "cmake --build --preset gcc-ship"])

    def test_bookkeeping_commands_are_not_recorded(self):
        self.sandbox.run("history")
        self.sandbox.run("doctor")
        self.sandbox.run("default")
        self.assertEqual(store.Store(self.sandbox.root).history(), [])

    def test_ranking_by_count_then_recency(self):
        entries = [store.Entry(["a"], 1, 30.0, True, 1.0), store.Entry(["b"], 5, 10.0, True, 1.0),
                   store.Entry(["c"], 5, 20.0, False, 1.0)]
        self.assertEqual([entry.argv for entry in store.rank_common(entries)], [["c"], ["b"], ["a"]])
        self.assertEqual([entry.argv for entry in store.rank_recent(entries)], [["a"], ["c"], ["b"]])

    def test_history_keeps_only_the_most_recent(self):
        keeper = store.Store(self.sandbox.root)
        for index in range(store.HISTORY_LIMIT + 5):
            keeper.record([f"build-{index}"], True, 1.0, now=float(index))
        entries = keeper.history()
        self.assertEqual(len(entries), store.HISTORY_LIMIT)
        self.assertNotIn(["build-0"], [entry.argv for entry in entries])

    def test_history_keeps_times_to_the_millisecond_and_durations_to_the_tenth(self):
        keeper = store.Store(self.sandbox.root)
        keeper.record(["build"], True, 23.886717182998837, now=1791612082.4157877)
        entry = store.Store(self.sandbox.root).history()[0]
        self.assertEqual((entry.last_run, entry.duration), (1791612082.416, 23.9))

    def test_corrupt_history_starts_over_instead_of_stopping_the_tool(self):
        folder = self.sandbox.root / ".assisi"
        folder.mkdir()
        (folder / "history.json").write_text("{not json")
        self.sandbox.configured("gcc-dev")
        self.assertEqual(self.sandbox.run("build", "dev")[0], 0)
        self.assertEqual([entry.argv for entry in store.Store(self.sandbox.root).history()],
                         [["build", "dev", "--compiler", "gcc"]])


class RecipeTest(ToolTest):
    def test_a_saved_recipe_runs_by_name(self):
        self.sandbox.configured("gcc-ship")
        self.sandbox.tools_built("gcc-ship")
        self.assertEqual(self.sandbox.run("save", "ship-it", "--", "package", "ship")[0], 0)
        self.assertEqual(self.sandbox.run("ship-it")[0], 0)
        self.assertIn("assisi-pack", " ".join(self.sandbox.calls()))
        self.assertEqual(store.Store(self.sandbox.root).recipes()["ship-it"],
                         ["package", "ship", "--compiler", "gcc"])

    def test_save_without_a_command_takes_the_last_one_run(self):
        self.sandbox.configured("gcc-dev")
        self.sandbox.run("build", "dev", "--game")
        self.assertEqual(self.sandbox.run("save", "game")[0], 0)
        self.assertEqual(store.Store(self.sandbox.root).recipes()["game"],
                         ["build", "dev", "--compiler", "gcc", "--game"])

    def test_names_that_mean_something_already_are_refused(self):
        for name in ("build", "dev", "gcc", "gcc-ship"):
            with self.subTest(name=name):
                self.assertNotEqual(self.sandbox.run("save", name, "--", "build", "dev")[0], 0)
        self.assertEqual(store.Store(self.sandbox.root).recipes(), {})

    def test_an_invalid_recipe_command_is_refused(self):
        code, _, _ = self.sandbox.run("save", "broken", "--", "build", "nonsense")
        self.assertNotEqual(code, 0)
        self.assertEqual(store.Store(self.sandbox.root).recipes(), {})

    def test_forget_removes_a_recipe(self):
        self.sandbox.run("save", "ship-it", "--", "package", "ship")
        self.assertEqual(self.sandbox.run("forget", "ship-it")[0], 0)
        self.assertEqual(store.Store(self.sandbox.root).recipes(), {})


def make_checkout(folder: Path) -> Path:
    """The two files that make a folder an Assisi checkout."""
    (folder / "tools" / "assisi").mkdir(parents=True)
    (folder / "tools" / "assisi" / "cli.py").write_text("")
    (folder / "assisi").write_text("")
    return folder


class LocateTest(unittest.TestCase):
    def setUp(self):
        self._directory = tempfile.TemporaryDirectory()
        self.top = Path(self._directory.name).resolve()

    def tearDown(self):
        self._directory.cleanup()

    def test_the_checkout_is_found_from_a_folder_inside_it(self):
        checkout = make_checkout(self.top / "game")
        inside = checkout / "apps" / "game" / "src"
        inside.mkdir(parents=True)
        self.assertEqual(locate.checkout_above(inside), checkout)

    def test_a_folder_with_only_a_file_called_assisi_is_not_a_checkout(self):
        (self.top / "assisi").write_text("")
        self.assertIsNone(locate.checkout_above(self.top))

    def test_the_command_hands_off_to_the_checkout_it_is_typed_in(self):
        own, other = make_checkout(self.top / "a"), make_checkout(self.top / "b")
        self.assertEqual(locate.handoff_target(own, other / "tools", True), other)

    def test_a_tool_typed_by_its_path_runs_its_own_checkout(self):
        own, other = make_checkout(self.top / "a"), make_checkout(self.top / "b")
        self.assertIsNone(locate.handoff_target(own, other, False))

    def test_the_command_never_hands_off_to_itself_or_outside_a_checkout(self):
        own = make_checkout(self.top / "a")
        self.assertIsNone(locate.handoff_target(own, own / "tools", True))
        self.assertIsNone(locate.handoff_target(own, self.top, True))

    def test_the_command_is_told_apart_from_a_typed_path(self):
        script = make_checkout(self.top / "a") / "assisi"
        link = self.top / "link"
        link.symlink_to(script)
        self.assertTrue(locate.via_command(link, {}))
        self.assertFalse(locate.via_command(script, {}))
        self.assertTrue(locate.via_command(script, {locate.COMMAND_VARIABLE: "1"}))


class InstallTest(unittest.TestCase):
    def setUp(self):
        self._directory = tempfile.TemporaryDirectory()
        self.top = Path(self._directory.name).resolve()
        self.bin = self.top / "bin"
        self.checkout = make_checkout(self.top / "a")
        self.asked: list[str] = []

    def tearDown(self):
        self._directory.cleanup()

    def install(self, answer: bool = True) -> bool:
        def confirm(question: str) -> bool:
            self.asked.append(question)
            return answer
        return install.install_posix(self.checkout, self.bin, confirm, lambda line: None)

    def test_the_command_links_to_the_checkout(self):
        self.assertTrue(self.install())
        self.assertEqual((self.bin / "assisi").resolve(), self.checkout / "assisi")

    def test_installing_again_asks_nothing(self):
        self.install()
        self.assertTrue(self.install())
        self.assertEqual(self.asked, [])

    def test_a_command_for_another_checkout_is_replaced_only_when_told(self):
        other = make_checkout(self.top / "b")
        self.bin.mkdir()
        (self.bin / "assisi").symlink_to(other / "assisi")
        self.assertFalse(self.install(answer=False))
        self.assertEqual((self.bin / "assisi").resolve(), other / "assisi")
        self.assertTrue(self.install(answer=True))
        self.assertEqual((self.bin / "assisi").resolve(), self.checkout / "assisi")

    def test_a_file_that_is_not_a_link_is_left_alone(self):
        self.bin.mkdir()
        (self.bin / "assisi").write_text("someone else's\n")
        self.assertFalse(self.install())
        self.assertEqual((self.bin / "assisi").read_text(), "someone else's\n")

    def test_path_advice_only_when_the_folder_is_not_on_path(self):
        self.assertIsNone(install.path_advice(self.bin, os.pathsep.join(["/usr/bin", str(self.bin)]), "/bin/zsh"))
        self.assertIsNotNone(install.path_advice(self.bin, "/usr/bin", "/bin/zsh"))

    def test_a_windows_path_gains_the_folder_once(self):
        self.assertEqual(install.path_with(r"C:\Windows", r"C:\Tools"), r"C:\Windows;C:\Tools")
        self.assertIsNone(install.path_with(r"C:\Windows;c:\tools\\", r"C:\Tools"))
        self.assertEqual(install.path_with("", r"C:\Tools"), r"C:\Tools")

    def test_a_windows_command_says_which_checkout_it_runs(self):
        text = install.cmd_text(Path(r"C:\games\Assisi"), Path(r"C:\Python\python.exe"))
        self.assertEqual(install.cmd_checkout(text), str(Path(r"C:\games\Assisi")))


class SetupTest(ToolTest):
    def setUp(self):
        super().setUp()
        self.home = Path(self._directory.name) / "home"
        self.home.mkdir()

    def run_answering(self, answers: list[str], *argv: str, interactive: bool = True):
        with mock.patch("builtins.input", side_effect=answers) as asked:
            code, out, err = self.sandbox.run(*argv, interactive=interactive, HOME=str(self.home))
        return code, asked.call_count

    def settings(self) -> dict:
        return project.Project(self.sandbox.root, host="Linux").read_settings()

    def test_the_first_run_sets_up_then_does_what_it_was_asked(self):
        code, asked = self.run_answering(["clang", "ship", "y"], "default")
        self.assertEqual(code, 0)
        self.assertEqual(asked, 3)
        settings = self.settings()
        self.assertEqual((settings[project.COMPILER_KEY], settings[project.LEVEL_KEY]), ("clang", "ship"))
        self.assertEqual((self.home / ".local" / "bin" / "assisi").resolve(), (self.sandbox.root / "assisi").resolve())

    def test_the_second_run_asks_nothing(self):
        self.run_answering(["", "", "n"], "default")
        code, asked = self.run_answering([], "default")
        self.assertEqual((code, asked), (0, 0))

    def test_enter_keeps_the_defaults_and_no_keeps_the_command_off(self):
        self.run_answering(["", "", "n"], "default")
        settings = self.settings()
        self.assertEqual((settings[project.COMPILER_KEY], settings[project.LEVEL_KEY]), ("gcc", "dev"))
        self.assertFalse((self.home / ".local" / "bin" / "assisi").exists())

    def test_off_a_terminal_nothing_is_asked_or_recorded(self):
        code, asked = self.run_answering([], "default", interactive=False)
        self.assertEqual((code, asked), (0, 0))
        self.assertNotIn(project.SETUP_KEY, self.settings())

    def test_inside_the_steam_runtime_container_nothing_is_asked(self):
        with mock.patch("builtins.input", side_effect=[]) as asked:
            self.sandbox.run("default", interactive=True, HOME=str(self.home), ASSISI_STEAMRT_SDK="1")
        self.assertEqual(asked.call_count, 0)

    def test_help_does_not_set_up(self):
        for argv in (["--help"], ["build", "--help"]):
            with self.subTest(argv=argv):
                self.assertEqual(self.run_answering([], *argv)[1], 0)

    def test_setup_runs_again_when_asked(self):
        self.run_answering(["", "", "n"], "default")
        code, asked = self.run_answering(["", "ship", "n"], "setup")
        self.assertEqual((code, asked), (0, 3))
        self.assertEqual(self.settings()[project.LEVEL_KEY], "ship")

    def test_setup_off_a_terminal_is_refused(self):
        self.assertNotEqual(self.run_answering([], "setup", interactive=False)[0], 0)

    def test_closing_input_at_the_first_question_still_counts_and_carries_on(self):
        code, asked = self.run_answering(EOFError(), "default")
        self.assertEqual(code, 0)
        self.assertTrue(self.settings()[project.SETUP_KEY])

    def test_an_answer_that_is_not_a_choice_is_asked_again(self):
        _, asked = self.run_answering(["msvc", "gcc", "dev", "n"], "default")
        self.assertEqual(asked, 4)
        self.assertEqual(self.settings()[project.COMPILER_KEY], "gcc")


class CompatibilityTest(unittest.TestCase):
    """The tool, TUI included, runs on the Steam Runtime container's Python 3.9
    with nothing installed beside it."""

    def tool_files(self) -> list[Path]:
        package = TOOLS / "assisi"
        return [path for path in package.rglob("*.py") if path.parent.name != "tests"]

    def test_tool_files_parse_as_python_3_9(self):
        for path in self.tool_files():
            with self.subTest(path=path.name):
                ast.parse(path.read_text(encoding="utf-8"), filename=str(path), feature_version=(3, 9))

    @unittest.skipUnless(hasattr(sys, "stdlib_module_names"), "Python 3.10 lists the standard library")
    def test_tool_imports_only_the_standard_library(self):
        allowed = set(sys.stdlib_module_names) | {"assisi", "__future__"}
        for path in self.tool_files():
            for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"), filename=str(path))):
                if isinstance(node, ast.Import):
                    names = [alias.name for alias in node.names]
                elif isinstance(node, ast.ImportFrom) and node.level == 0:
                    names = [node.module or ""]
                else:
                    continue
                for name in names:
                    self.assertIn(name.split(".")[0], allowed, f"{path.name} imports {name}")


if __name__ == "__main__":
    unittest.main()
