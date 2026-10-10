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

TOOLS = Path(__file__).resolve().parents[2]
REPO = TOOLS.parent
sys.path.insert(0, str(TOOLS))

from assisi import cli, project, request, store  # noqa: E402
from assisi.commands import catalog  # noqa: E402

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

    def run(self, *argv: str, **environment: str):
        env = {"PATH": str(self.bin), "FAKE_LOG": str(self.log)}
        env.update(environment)
        out, err = io.StringIO(), io.StringIO()
        saved = dict(os.environ)
        try:
            os.environ.clear()
            os.environ.update(env)
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = cli.main(list(argv), root=self.root, host="Linux", interactive=False)
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
        code, _, err = self.sandbox.run("package")
        self.assertNotEqual(code, 0)
        self.assertIn("ship", err)
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
        code, _, err = self.sandbox.run("build", "dev", "--compiler", "msvc")
        self.assertNotEqual(code, 0)
        self.assertIn("Windows", err)
        self.assertEqual(self.sandbox.calls(), [])

    def test_unknown_level_lists_the_levels(self):
        code, _, err = self.sandbox.run("build", "fast")
        self.assertNotEqual(code, 0)
        self.assertIn("dev", err)
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
        presets = json.loads((REPO / "CMakePresets.json").read_text())
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
            "ctest --preset gcc-ship-steamrt -L game|container",
        ])

    def test_fetch_and_remove(self):
        stale = self.sandbox.root / "out" / "steamrt-home"
        stale.mkdir(parents=True)
        self.assertEqual(self.sandbox.run("steamrt", "fetch")[0], 0)
        self.assertEqual(self.sandbox.run("steamrt", "remove")[0], 0)
        self.assertEqual(self.sandbox.calls(), ["steamrt prepare", "steamrt remove"])
        self.assertFalse(stale.exists())


class TestCommandTest(ToolTest):
    def test_builds_then_runs_ctest_with_passthrough(self):
        self.sandbox.configured("gcc-debug")
        code, _, err = self.sandbox.run("test", "debug", "--", "-R", "Audio")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.sandbox.calls(), ["cmake --build --preset gcc-debug",
                                                "ctest --preset gcc-debug -R Audio"])


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
        code, _, err = self.sandbox.run("clean")
        self.assertNotEqual(code, 0)
        self.assertIn("--all", err)
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

    def test_missing_tools_are_named_with_a_remedy(self):
        box = self.sandbox(())
        code, out, _ = box.run("doctor")
        self.assertNotEqual(code, 0)
        for name in ("cmake", "ninja", "git", "g++"):
            self.assertIn(name, out)

    def test_everything_present_passes(self):
        box = self.sandbox(PROGRAMS)
        code, out, _ = box.run("doctor")
        self.assertEqual(code, 0, out)

    def test_old_cmake_fails(self):
        box = self.sandbox(PROGRAMS)
        code, out, _ = box.run("doctor", FAKE_CMAKE_VERSION="3.20.0")
        self.assertNotEqual(code, 0)
        self.assertIn("3.28", out)

    def test_missing_static_libstdcxx_fails(self):
        box = self.sandbox(PROGRAMS)
        code, out, _ = box.run("doctor", FAKE_LIBSTDCXX="libstdc++.a")
        self.assertNotEqual(code, 0)
        self.assertIn("libstdc++", out)


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

    def test_game_without_a_package_says_how_to_make_one(self):
        self.sandbox.tools_built("gcc-dev")
        code, _, err = self.sandbox.run("run", "game", "dev", "--no-build")
        self.assertNotEqual(code, 0)
        self.assertIn("package", err)
        self.assertEqual(self.sandbox.calls(), [])


class RequestTest(ToolTest):
    EXAMPLES = (
        ["build", "dev", "--game"],
        ["build", "debug", "dev", "ship", "--compiler", "clang"],
        ["build", "debug", "--sanitize", "thread", "--profiler"],
        ["package", "ship"],
        ["package", "ship", "--steam-runtime"],
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

    def test_corrupt_history_starts_empty_and_says_so(self):
        folder = self.sandbox.root / ".assisi"
        folder.mkdir()
        (folder / "history.json").write_text("{not json")
        code, out, err = self.sandbox.run("history")
        self.assertEqual(code, 0)
        self.assertIn("history.json", err)


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
                code, _, err = self.sandbox.run("save", name, "--", "build", "dev")
                self.assertNotEqual(code, 0)
                self.assertIn(name, err)
        self.assertEqual(store.Store(self.sandbox.root).recipes(), {})

    def test_an_invalid_recipe_command_is_refused(self):
        code, _, _ = self.sandbox.run("save", "broken", "--", "build", "nonsense")
        self.assertNotEqual(code, 0)
        self.assertEqual(store.Store(self.sandbox.root).recipes(), {})

    def test_forget_removes_a_recipe(self):
        self.sandbox.run("save", "ship-it", "--", "package", "ship")
        self.assertEqual(self.sandbox.run("forget", "ship-it")[0], 0)
        self.assertEqual(store.Store(self.sandbox.root).recipes(), {})


class DispatchTest(ToolTest):
    def test_no_arguments_off_a_terminal_prints_help(self):
        code, out, _ = self.sandbox.run()
        self.assertEqual(code, 0)
        self.assertIn("package", out)
        self.assertNotIn("textual", sys.modules)

    def test_unknown_command_names_the_commands(self):
        code, _, err = self.sandbox.run("frobnicate")
        self.assertNotEqual(code, 0)
        self.assertIn("build", err)


class CompatibilityTest(unittest.TestCase):
    """The command line runs on the Steam Runtime container's Python 3.9."""

    def cli_files(self) -> list[Path]:
        package = TOOLS / "assisi"
        return [path for path in package.rglob("*.py")
                if path.parent.name not in ("tui", "tests") or path.name == "model.py"]

    def test_command_line_files_parse_as_python_3_9(self):
        for path in self.cli_files():
            with self.subTest(path=path.name):
                ast.parse(path.read_text(), filename=str(path), feature_version=(3, 9))

    def test_command_line_never_imports_textual(self):
        for path in self.cli_files():
            text = path.read_text()
            for statement in ("import textual", "from textual", "import rich", "from rich"):
                self.assertFalse(statement in text, f"{path.name} has '{statement}'")


if __name__ == "__main__":
    unittest.main()
