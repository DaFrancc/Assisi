#!/usr/bin/env python3
"""Tests for the TUI: what each key and form asks for, and that the app does it.

The model tests need nothing but Python and run everywhere. The app tests drive
the real Textual app headless through its test pilot, with an executor that
records requests instead of running them; they need the TUI's environment and
skip without it, so a machine that never opened the TUI still passes.
"""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))

from assisi import core  # noqa: E402
from assisi.process import Runner  # noqa: E402
from assisi.project import Project, UsageError  # noqa: E402
from assisi.store import Store  # noqa: E402
from assisi.tui import model  # noqa: E402

HAVE_TEXTUAL = importlib.util.find_spec("textual") is not None

# Large enough that every pane and the footer are on screen.
SCREEN_SIZE = (140, 40)


def make_context(root: Path) -> core.Context:
    return core.Context(Project(root, host="Linux"), Runner(root), Store(root))


class ModelTest(unittest.TestCase):
    def setUp(self):
        self._directory = tempfile.TemporaryDirectory()
        self.root = Path(self._directory.name)
        self.context = make_context(self.root)

    def tearDown(self):
        self._directory.cleanup()

    def preview_for_key(self, key: str, tree: str) -> str:
        form = model.form_for_build_key(key, tree, self.context.project)
        return form.preview()[1]

    def test_build_keys_open_their_command_set_to_the_row(self):
        self.assertEqual(self.preview_for_key("p", "gcc-ship"), "./assisi package ship --compiler gcc")
        self.assertEqual(self.preview_for_key("b", "clang-dev"), "./assisi build dev --compiler clang")
        self.assertEqual(self.preview_for_key("e", "gcc-asan"),
                         "./assisi run editor debug --compiler gcc --sanitize address")
        self.assertEqual(self.preview_for_key("r", "gcc-dev"), "./assisi run game dev --compiler gcc")
        self.assertEqual(self.preview_for_key("p", "gcc-ship-steamrt"),
                         "./assisi package ship --compiler gcc --steam-runtime")
        self.assertEqual(self.preview_for_key("k", "gcc-ship-chiara"), "./assisi cook ship --compiler gcc --profiler")

    def test_the_release_key_opens_a_form_waiting_for_its_version(self):
        form = model.form_for_build_key("v", "gcc-ship", self.context.project)
        self.assertFalse(form.preview()[0])
        form.set("version", "1.0")
        self.assertEqual(form.preview(), (True, "./assisi release ship --compiler gcc --version 1.0"))

    def test_a_key_that_does_not_apply_to_the_build_is_refused(self):
        for key in ("k", "p", "v"):
            with self.subTest(key=key):
                with self.assertRaises(UsageError):
                    model.form_for_build_key(key, "gcc-asan", self.context.project)

    def test_the_form_previews_exactly_the_command_line_it_runs(self):
        from assisi.commands import by_name
        form = model.FormModel(by_name()["build"], self.context.project)
        self.assertEqual(form.preview(), (True, "./assisi build dev --compiler gcc"))
        form.set("game", True)
        form.set("levels", "ship")
        form.set("compiler", "clang")
        ok, text = form.preview()
        self.assertTrue(ok)
        self.assertEqual(text, model.command_line(form.request().to_argv()))
        self.assertEqual(text, "./assisi build ship --compiler clang --game")

    def test_text_fields_become_their_flags(self):
        from assisi.commands import by_name
        form = model.FormModel(by_name()["release"], self.context.project)
        self.assertFalse(form.preview()[0])
        form.set("version", "1.2")
        form.set("level", "ship")
        self.assertEqual(form.request().to_argv(), ["release", "ship", "--compiler", "gcc", "--version", "1.2"])

    def test_a_form_for_a_build_with_no_preset_says_so(self):
        from assisi.commands import by_name
        form = model.FormModel(by_name()["build"], self.context.project)
        form.set("sanitize", "address")
        ok, text = form.preview()
        self.assertFalse(ok)
        self.assertIn("debug", text)

    def test_a_form_that_cannot_run_says_why(self):
        from assisi.commands import by_name
        form = model.FormModel(by_name()["clean"], self.context.project)
        ok, text = form.preview()
        self.assertFalse(ok)
        self.assertIn("--all", text)

    def test_passthrough_is_split_like_a_shell(self):
        from assisi.commands import by_name
        form = model.FormModel(by_name()["run"], self.context.project)
        form.set("program_arguments", "-l 'levels/My Level.alvl'")
        self.assertEqual(form.request().to_argv(),
                         ["run", "editor", "dev", "--compiler", "gcc", "--", "-l", "levels/My Level.alvl"])

    def test_rows_mark_the_default_build_and_read_status_from_disk(self):
        self.context.project.write_settings({"compiler": "clang", "level": "dev"})
        build = self.context.project.build_dir("clang-dev")
        build.mkdir(parents=True)
        (build / "CMakeCache.txt").write_text("")
        rows = {row.tree: row for row in model.build_rows(self.context.project)}
        self.assertTrue(rows["clang-dev"].is_default)
        self.assertFalse(rows["gcc-dev"].is_default)
        self.assertEqual(rows["clang-dev"].glyphs()[0][0], "●")
        self.assertEqual(rows["gcc-dev"].glyphs()[0][0], "○")

    def test_frequent_is_most_used_first_and_capped(self):
        store = self.context.store
        for index in range(model.FREQUENT_LIMIT + 3):
            store.record(["build", f"tree-{index}"], True, 1.0, now=float(index))
        store.record(["build", "tree-0"], True, 1.0, now=100.0)
        entries = model.frequent(store)
        self.assertEqual(len(entries), model.FREQUENT_LIMIT)
        self.assertEqual(entries[0].argv, ["build", "tree-0"])

    def test_durations_read_as_minutes_and_seconds(self):
        self.assertEqual(model.format_duration(7.4), "7s")
        self.assertEqual(model.format_duration(134.0), "2m14s")


@unittest.skipUnless(HAVE_TEXTUAL, "the TUI's environment is not installed")
class AppTest(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self._directory = tempfile.TemporaryDirectory()
        self.root = Path(self._directory.name)
        self.context = make_context(self.root)
        self.ran = []

    def tearDown(self):
        self._directory.cleanup()

    def executor(self, request):
        self.ran.append(request.to_argv())
        return 0, 1.0

    def app(self):
        from assisi.tui.app import AssisiApp
        return AssisiApp(self.context, executor=self.executor)

    async def test_enter_on_a_recipe_runs_it(self):
        self.context.store.save_recipe("ship-it", ["package", "ship", "--compiler", "gcc"])
        app = self.app()
        async with app.run_test(size=SCREEN_SIZE) as pilot:
            recipes = app.query_one("#recipes")
            recipes.focus()
            recipes.highlighted = 0
            await pilot.press("enter")
        self.assertEqual(self.ran, [["package", "ship", "--compiler", "gcc"]])

    async def test_a_key_on_a_build_row_opens_its_menu_and_runs_nothing_until_asked(self):
        app = self.app()
        async with app.run_test(size=SCREEN_SIZE) as pilot:
            from assisi.tui.app import BuildTable, FormScreen
            table = app.query_one(BuildTable)
            table.move_cursor(row=table.get_row_index("gcc-ship"))
            await pilot.press("p")
            await pilot.pause()
            self.assertIsInstance(app.screen, FormScreen)
            self.assertEqual(str(app.screen.query_one("#command-bar").content), "./assisi package ship --compiler gcc")
            self.assertEqual(self.ran, [])
            await pilot.press("ctrl+r")
            await pilot.pause()
        self.assertEqual(self.ran, [["package", "ship", "--compiler", "gcc"]])

    async def test_escape_leaves_a_menu_without_running_it(self):
        app = self.app()
        async with app.run_test(size=SCREEN_SIZE) as pilot:
            from assisi.tui.app import BuildTable
            table = app.query_one(BuildTable)
            table.move_cursor(row=table.get_row_index("gcc-dev"))
            await pilot.press("k")
            await pilot.pause()
            await pilot.press("escape")
            await pilot.pause()
            self.assertIsInstance(app.screen.query_one(BuildTable), BuildTable)
        self.assertEqual(self.ran, [])

    async def test_the_command_bar_follows_the_form(self):
        app = self.app()
        async with app.run_test(size=SCREEN_SIZE) as pilot:
            from assisi.tui.app import BuildTable
            table = app.query_one(BuildTable)
            table.move_cursor(row=table.get_row_index("gcc-dev"))
            await pilot.press("b")
            await pilot.pause()
            app.screen.query_one("#field-game").focus()
            await pilot.press("space")
            await pilot.pause()
            self.assertEqual(str(app.screen.query_one("#command-bar").content),
                             "./assisi build dev --compiler gcc --game")
            await pilot.press("ctrl+r")
            await pilot.pause()
        self.assertEqual(self.ran, [["build", "dev", "--compiler", "gcc", "--game"]])

    async def test_choosing_from_a_form_list_only_changes_the_form(self):
        # A form's dropdown is a list too; picking from it must change that
        # option and nothing else, not be taken for a command to run.
        app = self.app()
        async with app.run_test(size=SCREEN_SIZE) as pilot:
            from assisi.tui.app import BuildTable
            table = app.query_one(BuildTable)
            table.move_cursor(row=table.get_row_index("gcc-debug"))
            await pilot.press("b")
            await pilot.pause()
            app.screen.query_one("#field-sanitize").focus()
            await pilot.press("enter")
            await pilot.pause()
            # The list opens on its blank "none"; address is the next entry.
            await pilot.press("down", "enter")
            await pilot.pause()
            self.assertEqual(str(app.screen.query_one("#command-bar").content),
                             "./assisi build debug --compiler gcc --sanitize address")
        self.assertEqual(self.ran, [])

    async def test_saving_refuses_a_taken_name_and_keeps_a_good_one(self):
        app = self.app()
        async with app.run_test(size=SCREEN_SIZE) as pilot:
            from assisi.tui.app import BuildTable
            table = app.query_one(BuildTable)
            table.move_cursor(row=table.get_row_index("gcc-dev"))
            await pilot.press("s")
            await pilot.pause()
            await pilot.press(*"build", "enter")
            await pilot.pause()
            self.assertEqual(self.context.store.recipes(), {})
            self.assertIn("command", str(app.screen.query_one("#recipe-problem").content))
            name = app.screen.query_one("#recipe-name")
            name.value = ""
            await pilot.press(*"daily", "enter")
            await pilot.pause()
        self.assertEqual(self.context.store.recipes(), {"daily": ["build", "dev", "--compiler", "gcc"]})


if __name__ == "__main__":
    unittest.main()
