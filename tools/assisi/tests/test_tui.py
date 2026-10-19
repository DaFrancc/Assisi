#!/usr/bin/env python3
"""Tests for the TUI: what each key and form asks for, and that the app does it.

The app runs against a fake terminal that hands it scripted keys and a fixed
size, with an executor that records requests instead of running them, so none
of this needs a real terminal. The tests of the real terminal's mode use a
pseudo-terminal and only run where there are pseudo-terminals.
"""

from __future__ import annotations

import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Iterator, List, Optional, Tuple
from unittest import mock

TOOLS = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))

from assisi import core  # noqa: E402
from assisi.commands import by_name  # noqa: E402
from assisi.process import Runner  # noqa: E402
from assisi.project import COMPILER_KEY, LEVEL_KEY, Project, UsageError  # noqa: E402
from assisi.request import parse  # noqa: E402
from assisi.store import Store  # noqa: E402
from assisi.tui import files, keys, model  # noqa: E402
from assisi.tui.app import INTERRUPTED_CODE, App, Browser, Confirm, Form, Home  # noqa: E402
from assisi.tui.canvas import Canvas, Color, Style, render  # noqa: E402

# Large enough that every pane and the status lines are on screen.
SCREEN_SIZE = (140, 40)

# A terminal as small as a default one, where the panes still sit side by side.
SMALL_SIZE = (80, 24)

# Narrower than the side-by-side layout allows, so the panes stack.
NARROW_SIZE = (60, 40)

# More recipes than the recipes pane of a small terminal has rows for.
MANY_RECIPES = 30

HAVE_PTY = hasattr(os, "openpty")


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
        self.assertEqual(self.preview_for_key("c", "gcc-ship-chiara"), "./assisi cook ship --compiler gcc --profiler")

    def test_the_release_key_opens_a_form_waiting_for_its_version(self):
        form = model.form_for_build_key("v", "gcc-ship", self.context.project)
        self.assertFalse(form.preview()[0])
        form.set("version", "1.0")
        self.assertEqual(form.preview(), (True, "./assisi release ship --compiler gcc --version 1.0"))

    def test_a_key_that_does_not_apply_to_the_build_is_refused(self):
        for key in ("c", "p", "v"):
            with self.subTest(key=key):
                with self.assertRaises(UsageError):
                    model.form_for_build_key(key, "gcc-asan", self.context.project)

    def test_the_form_previews_exactly_the_command_line_it_runs(self):
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
        form = model.FormModel(by_name()["release"], self.context.project)
        self.assertFalse(form.preview()[0])
        form.set("version", "1.2")
        form.set("level", "ship")
        self.assertEqual(form.request().to_argv(), ["release", "ship", "--compiler", "gcc", "--version", "1.2"])

    def test_a_form_for_a_build_with_no_preset_says_so(self):
        form = model.FormModel(by_name()["build"], self.context.project)
        form.set("sanitize", "address")
        ok, text = form.preview()
        self.assertFalse(ok)
        self.assertIn("debug", text)

    def test_a_form_that_cannot_run_says_why(self):
        form = model.FormModel(by_name()["clean"], self.context.project)
        ok, text = form.preview()
        self.assertFalse(ok)
        self.assertIn("--all", text)

    def test_passthrough_is_split_like_a_shell(self):
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

    def complete(self, text: str) -> Tuple[str, List[str]]:
        return model.complete(text, self.context.store, self.context.project)

    def test_completion_finishes_a_command_name(self):
        self.assertEqual(self.complete("packa")[0], "package ")

    def test_completion_stops_where_candidates_part(self):
        text, candidates = self.complete("c")
        self.assertEqual(text, "c")
        self.assertIn("clean", candidates)
        self.assertIn("cook", candidates)

    def test_completion_offers_recipes_and_the_typed_commands_flags(self):
        self.context.store.save_recipe("ship-it", ["package", "ship", "--compiler", "gcc"])
        self.assertEqual(self.complete("ship-")[0], "ship-it ")
        self.assertEqual(self.complete("build dev --ga")[0], "build dev --game ")
        self.assertEqual(self.complete("build dev --compiler cl")[0], "build dev --compiler clang ")

    def test_a_recipe_named_at_the_command_line_runs_it(self):
        self.context.store.save_recipe("ship-it", ["package", "ship", "--compiler", "gcc"])
        request = model.line_request("ship-it", self.context.store, self.context.project)
        self.assertEqual(request.to_argv(), ["package", "ship", "--compiler", "gcc"])

    def test_the_command_line_takes_the_launcher_out_of_habit(self):
        request = model.line_request("./assisi build ship", self.context.store, self.context.project)
        self.assertEqual(request.to_argv(), ["build", "ship", "--compiler", "gcc"])


CAN_LOCK = os.name == "posix" and hasattr(os, "geteuid") and os.geteuid() != 0


class FilesTest(unittest.TestCase):
    def setUp(self):
        self._directory = tempfile.TemporaryDirectory()
        self.root = Path(self._directory.name)
        self.project = Project(self.root, host="Linux")

    def tearDown(self):
        for path in self.root.rglob("*"):
            path.chmod(0o755)
        self._directory.cleanup()

    def test_folders_come_first_under_the_way_up(self):
        (self.root / "b.txt").write_text("")
        (self.root / "a").mkdir()
        (self.root / "c").mkdir()
        names = [entry.name for entry in files.listing(self.root)]
        self.assertEqual(names, ["..", "a", "c", "b.txt"])

    @unittest.skipUnless(CAN_LOCK, "needs a user permissions apply to")
    def test_what_cannot_be_opened_is_marked(self):
        locked = self.root / "locked"
        locked.mkdir()
        locked.chmod(0)
        (self.root / "open").mkdir()
        readable = {entry.name: entry.readable for entry in files.listing(self.root)}
        self.assertFalse(readable["locked"])
        self.assertTrue(readable["open"])

    def test_the_browser_starts_among_the_releases(self):
        self.assertEqual(files.start_directory(self.project, ""), self.root)
        (self.root / "releases" / "1.0").mkdir(parents=True)
        self.assertEqual(files.start_directory(self.project, ""), self.root / "releases")
        self.assertEqual(files.start_directory(self.project, "releases/1.0/assets.pak"),
                         self.root / "releases" / "1.0")

    def test_a_chosen_file_is_written_as_the_command_line_would_take_it(self):
        self.assertEqual(files.as_given(self.project, self.root / "releases" / "1.0" / "assets.pak"),
                         str(Path("releases") / "1.0" / "assets.pak"))
        outside = Path(tempfile.gettempdir()).resolve() / "elsewhere.pak"
        self.assertEqual(files.as_given(self.project, outside), str(outside))


class KeysTest(unittest.TestCase):
    def test_an_escape_sequence_is_one_key(self):
        self.assertEqual(keys.decode(b"\x1b[A"), (["up"], b""))
        self.assertEqual(keys.decode(b"\x1bOB"), (["down"], b""))
        self.assertEqual(keys.decode(b"\x1b[3~"), (["delete"], b""))
        self.assertEqual(keys.decode(b"\x1b[1;5C"), (["right"], b""))

    def test_a_sequence_split_across_reads_waits_for_the_rest(self):
        names, pending = keys.decode(b"j\x1b[")
        self.assertEqual((names, pending), (["j"], b"\x1b["))
        self.assertEqual(keys.decode(pending + b"A"), (["up"], b""))

    def test_a_lone_escape_waits_to_see_whether_a_sequence_follows(self):
        self.assertEqual(keys.decode(b"\x1b"), ([], b"\x1b"))
        self.assertEqual(keys.flush(b"\x1b"), ["esc"])

    def test_a_character_of_several_bytes_is_one_key(self):
        encoded = "é".encode("utf-8")
        self.assertEqual(keys.decode(encoded), (["é"], b""))
        self.assertEqual(keys.decode(encoded[:1]), ([], encoded[:1]))

    def test_control_keys_have_names(self):
        self.assertEqual(keys.decode(b"\x12\x7f\r\t\x15"), (["ctrl+r", "backspace", "enter", "tab", "ctrl+u"], b""))

    def test_windows_scan_codes_name_the_same_keys(self):
        self.assertEqual(keys.windows_key("H"), "up")
        self.assertEqual(keys.windows_key("S"), "delete")
        self.assertEqual(keys.char_key("\r"), "enter")
        self.assertEqual(keys.char_key("\x1b"), "esc")


class CanvasTest(unittest.TestCase):
    def test_text_past_the_right_edge_is_cut_off(self):
        canvas = Canvas(5, 2)
        canvas.put(3, 0, "abcdef")
        self.assertEqual(canvas.text_lines(), ["   ab", "     "])

    def test_an_unchanged_frame_writes_nothing(self):
        frame = Canvas(10, 3)
        frame.put(0, 1, "hello")
        again = Canvas(10, 3)
        again.put(0, 1, "hello")
        self.assertEqual(render(again, frame), "")

    def test_only_a_changed_line_is_written(self):
        frame = Canvas(10, 3)
        changed = Canvas(10, 3)
        changed.put(0, 2, "x", Style(color=Color.RED))
        output = render(changed, frame)
        self.assertIn("\x1b[3;1H", output)
        self.assertEqual(output.count("H"), 1)

    def test_a_first_frame_or_a_new_size_is_written_whole(self):
        for previous in (None, Canvas(9, 3)):
            with self.subTest(previous=previous):
                output = render(Canvas(10, 3), previous)
                for row in range(1, 4):
                    self.assertIn(f"\x1b[{row};1H", output)


class FakeTerminal:
    """Hands the app scripted keys at a fixed size, and records whether a
    session or a suspension is open."""

    class ScriptEnd(Exception):
        """Raised when the app asks for a key the script does not have."""

    def __init__(self, size: Tuple[int, int]):
        self._size = size
        self.keys: List[str] = []
        self.depth = 0
        self.suspensions = 0
        self.written: List[str] = []

    def size(self) -> Tuple[int, int]:
        return self._size

    def read_key(self, timeout: float) -> Optional[str]:
        if not self.keys:
            raise FakeTerminal.ScriptEnd()
        return self.keys.pop(0)

    def write(self, text: str) -> None:
        self.written.append(text)

    @contextlib.contextmanager
    def session(self) -> Iterator[None]:
        self.depth += 1
        try:
            yield
        finally:
            self.depth -= 1

    @contextlib.contextmanager
    def suspended(self) -> Iterator[None]:
        self.suspensions += 1
        self.depth -= 1
        try:
            yield
        finally:
            self.depth += 1


def typed(text: str) -> List[str]:
    return list(text)


class AppTest(unittest.TestCase):
    def setUp(self):
        self._directory = tempfile.TemporaryDirectory()
        self.root = Path(self._directory.name)
        self.context = make_context(self.root)
        self.ran: List[List[str]] = []

    def tearDown(self):
        self._directory.cleanup()

    def executor(self, request):
        self.ran.append(request.to_argv())
        return 0, 1.0

    def app(self, size: Tuple[int, int] = SCREEN_SIZE) -> App:
        return App(self.context, FakeTerminal(size), executor=self.executor)

    def press(self, app: App, *names: str) -> App:
        app.terminal.keys.extend(names)
        with contextlib.suppress(FakeTerminal.ScriptEnd):
            app.loop()
        return app

    def screen(self, app: App) -> str:
        return "\n".join(app.frame().text_lines())

    def to_build(self, tree: str) -> List[str]:
        """The keys that put the builds pane's cursor on @p tree."""
        trees = [row.tree for row in model.build_rows(self.context.project)]
        return ["g"] + ["j"] * trees.index(tree)

    def to_field(self, command: str, name: str) -> List[str]:
        """The keys that move a fresh form of @p command onto its field @p name."""
        names = [option.name for option in model.form_options(by_name()[command])]
        return ["j"] * names.index(name)

    def to_go(self, command: str) -> List[str]:
        """The keys that move a fresh form of @p command onto its Go button."""
        return ["j"] * len(model.form_options(by_name()[command]))

    def test_enter_on_a_recipe_runs_it(self):
        self.context.store.save_recipe("ship-it", ["package", "ship", "--compiler", "gcc"])
        self.press(self.app(), "tab", "enter")
        self.assertEqual(self.ran, [["package", "ship", "--compiler", "gcc"]])

    def test_a_key_on_a_build_row_opens_its_menu_and_runs_nothing_until_asked(self):
        app = self.press(self.app(), *self.to_build("gcc-ship"), "p")
        self.assertIsInstance(app.views[-1], Form)
        self.assertIn("./assisi package ship --compiler gcc", self.screen(app))
        self.assertEqual(self.ran, [])
        self.press(app, "ctrl+r")
        self.assertEqual(self.ran, [["package", "ship", "--compiler", "gcc"]])
        self.assertIsInstance(app.views[-1], Home)

    def test_a_menu_is_a_box_with_nothing_behind_showing_through(self):
        app = self.press(self.app(), *self.to_build("gcc-dev"), "b")
        lines = app.frame().text_lines()
        top = next(index for index, line in enumerate(lines) if "┌ build " in line)
        left = lines[top].index("┌ build ")
        right = lines[top].index("┐", left)
        bottom = next(index for index in range(top + 1, len(lines)) if lines[index][left] == "└")
        self.assertGreater(left, 0)
        self.assertLess(right, SCREEN_SIZE[0] - 1)
        inside = [line[left + 1:right] for line in lines[top + 1:bottom]]
        self.assertTrue(any("game" in line for line in inside))
        for line in inside:
            self.assertNotIn("○", line)
            self.assertNotIn("│", line)

    def test_a_menu_opens_over_the_home_screen(self):
        self.context.store.save_recipe("ship-it", ["package", "ship", "--compiler", "gcc"])
        app = self.press(self.app(), *self.to_build("gcc-dev"), "b")
        text = self.screen(app)
        self.assertIn("┌ build ", text)
        self.assertIn("ship-it", text)

    def test_a_long_entry_stays_inside_its_pane(self):
        self.context.store.save_recipe("long", ["test", "dev", "--compiler", "gcc", "--", "-R", "x" * SCREEN_SIZE[0]])
        self.context.store.record(["test", "dev", "--compiler", "gcc", "--", "-R", "y" * SCREEN_SIZE[0]], True, 1.0)
        lines = self.press(self.app()).frame().text_lines()
        header = next(line for line in lines if "Level" in line)
        self.assertIn("│ Level  Compiler", header)
        for line in lines:
            self.assertNotIn("│x", line)
            self.assertNotIn("│y", line)

    def test_go_at_the_bottom_of_a_menu_runs_it(self):
        app = self.press(self.app(), *self.to_build("gcc-ship"), "p", *self.to_go("package"))
        self.assertIn("[ Go ]", self.screen(app))
        self.assertEqual(self.ran, [])
        self.press(app, "enter")
        self.assertEqual(self.ran, [["package", "ship", "--compiler", "gcc"]])

    def test_go_on_a_menu_that_cannot_run_says_why_and_stays(self):
        app = self.press(self.app(), *self.to_build("gcc-ship"), "v", *self.to_go("release"), "enter")
        self.assertEqual(self.ran, [])
        self.assertIsInstance(app.views[-1], Form)
        self.assertIn("version", self.screen(app).splitlines()[-1])

    def make_release(self, version: str) -> Path:
        pak = self.root / "releases" / version / "assets.pak"
        pak.parent.mkdir(parents=True)
        pak.write_text("")
        return pak

    def open_browser(self) -> App:
        return self.press(self.app(), *self.to_build("gcc-ship"), "p", *self.to_field("package", "previous"), "enter")

    def test_a_file_setting_opens_a_browser_among_the_releases(self):
        self.make_release("1.0")
        app = self.open_browser()
        self.assertIsInstance(app.views[-1], Browser)
        self.assertIn("1.0/", self.screen(app))

    def test_choosing_a_file_in_the_browser_sets_it(self):
        self.make_release("1.0")
        app = self.press(self.open_browser(), "l", "enter")
        self.assertIsInstance(app.views[-1], Form)
        pak = str(Path("releases") / "1.0" / "assets.pak")
        self.assertIn(f"--previous {pak}", self.screen(app))
        self.press(app, *self.to_go("package")[self.to_field("package", "previous").count("j"):], "enter")
        self.assertEqual(self.ran, [["package", "ship", "--compiler", "gcc", "--previous", pak]])

    def test_the_browser_goes_up_and_esc_leaves_the_setting_alone(self):
        self.make_release("1.0")
        app = self.press(self.open_browser(), "h")
        self.assertIn("releases/", self.screen(app))
        self.press(app, "esc")
        self.assertIsInstance(app.views[-1], Form)
        self.assertNotIn("--previous", self.screen(app))

    @unittest.skipUnless(CAN_LOCK, "needs a user permissions apply to")
    def test_the_browser_will_not_open_what_it_cannot_read(self):
        self.make_release("1.0")
        locked = self.root / "releases" / "0.9"
        locked.mkdir()
        locked.chmod(0)
        try:
            app = self.press(self.open_browser(), "g", "j", "enter")
            self.assertIn("✘ 0.9/", self.screen(app))
            self.assertIn("1.0/", self.screen(app))
        finally:
            locked.chmod(0o755)

    def test_the_bottom_line_names_every_build_key_on_a_build(self):
        for size in (SCREEN_SIZE, SMALL_SIZE):
            with self.subTest(size=size):
                bottom = self.screen(self.press(self.app(size))).splitlines()[-1]
                for hint in ("b build", "c cook", "p package", "v release", "e editor", "r game", "t test",
                             "? keys"):
                    self.assertIn(hint, bottom)

    def test_the_bottom_line_follows_the_pane(self):
        bottom = self.screen(self.press(self.app(), "tab")).splitlines()[-1]
        self.assertIn("d delete", bottom)
        self.assertNotIn("v release", bottom)

    def test_a_narrow_bottom_line_keeps_the_key_help(self):
        bottom = self.screen(self.press(self.app(NARROW_SIZE))).splitlines()[-1]
        self.assertIn("? keys", bottom)

    def test_a_file_setting_shows_its_whole_path_under_its_description(self):
        pak = "releases/1.0/a-long-folder-name/assets.pak"
        app = self.press(self.app(), *self.to_build("gcc-ship"), "p", *self.to_field("package", "previous"),
                         "i", *typed(pak), "esc")
        lines = self.screen(app).splitlines()
        described = next(index for index, line in enumerate(lines) if "lay the pak out against" in line)
        self.assertIn(pak, lines[described + 1])
        self.assertNotIn(pak, lines[described])
        self.assertGreater(lines[described + 1].index(pak), lines[described].index("previous"))

    def test_escape_leaves_a_menu_without_running_it(self):
        app = self.press(self.app(), *self.to_build("gcc-dev"), "c", "esc")
        self.assertIsInstance(app.views[-1], Home)
        self.assertEqual(self.ran, [])

    def test_the_command_bar_follows_the_form(self):
        app = self.press(self.app(), *self.to_build("gcc-dev"), "b", *self.to_field("build", "game"), " ")
        self.assertIn("./assisi build dev --compiler gcc --game", self.screen(app))
        self.press(app, "ctrl+r")
        self.assertEqual(self.ran, [["build", "dev", "--compiler", "gcc", "--game"]])

    def test_choosing_a_value_only_changes_the_form(self):
        app = self.press(self.app(), *self.to_build("gcc-debug"), "b", *self.to_field("build", "sanitize"), "l")
        self.assertIn("./assisi build debug --compiler gcc --sanitize address", self.screen(app))
        self.assertEqual(self.ran, [])

    def test_a_text_field_is_typed_into(self):
        app = self.press(self.app(), *self.to_build("gcc-ship"), "v", *self.to_field("release", "version"),
                         "i", *typed("1.0"), "esc")
        self.assertIn("./assisi release ship --compiler gcc --version 1.0", self.screen(app))
        self.assertIsInstance(app.views[-1], Form)

    def test_saving_refuses_a_taken_name_and_keeps_a_good_one(self):
        app = self.press(self.app(), *self.to_build("gcc-dev"), "s", *typed("build"), "enter")
        self.assertEqual(self.context.store.recipes(), {})
        self.assertIn("command", self.screen(app))
        self.press(app, "ctrl+u", *typed("daily"), "enter")
        self.assertEqual(self.context.store.recipes(), {"daily": ["build", "dev", "--compiler", "gcc"]})

    def test_the_command_line_runs_what_it_is_given(self):
        self.press(self.app(), ":", *typed("package ship --compiler gcc"), "enter")
        self.assertEqual(self.ran, [["package", "ship", "--compiler", "gcc"]])

    def test_the_command_line_says_what_is_wrong_and_runs_nothing(self):
        app = self.press(self.app(), ":", *typed("frobnicate"), "enter")
        self.assertEqual(self.ran, [])
        self.assertIn("not a command", self.screen(app))

    def test_a_command_name_alone_opens_its_form(self):
        app = self.press(self.app(), ":", *typed("build"), "enter")
        self.assertIsInstance(app.views[-1], Form)
        self.assertEqual(self.ran, [])

    def test_tab_completes_on_the_command_line(self):
        app = self.press(self.app(), ":", *typed("packa"), "tab")
        self.assertIn(":package ", self.screen(app))

    def test_something_that_deletes_asks_first(self):
        expected = parse(["clean", "--all"], self.context.project).to_argv()
        app = self.press(self.app(), ":", *typed("clean --all"), "enter")
        self.assertIsInstance(app.views[-1], Confirm)
        self.press(app, "n")
        self.assertEqual(self.ran, [])
        self.press(app, ":", *typed("clean --all"), "enter", "y")
        self.assertEqual(self.ran, [expected])

    def test_capital_d_makes_the_row_the_default(self):
        self.press(self.app(), *self.to_build("clang-ship"), "D")
        settings = self.context.project.read_settings()
        self.assertEqual((settings[LEVEL_KEY], settings[COMPILER_KEY]), ("ship", "clang"))

    def test_deleting_a_recipe_asks_first(self):
        self.context.store.save_recipe("ship-it", ["package", "ship", "--compiler", "gcc"])
        app = self.press(self.app(), "tab", "d")
        self.assertIn("ship-it", self.context.store.recipes())
        self.press(app, "y")
        self.assertEqual(self.context.store.recipes(), {})

    def test_the_filter_hides_what_does_not_match(self):
        self.context.store.save_recipe("ship-it", ["package", "ship", "--compiler", "gcc"])
        self.context.store.save_recipe("daily", ["build", "dev", "--compiler", "gcc"])
        app = self.press(self.app(), "/", *typed("ship-"), "enter")
        text = self.screen(app)
        self.assertIn("ship-it", text)
        self.assertNotIn("daily", text)

    def test_a_number_runs_that_most_used_command(self):
        self.context.store.record(["build", "ship", "--compiler", "gcc"], True, 1.0)
        self.press(self.app(), "1")
        self.assertEqual(self.ran, [["build", "ship", "--compiler", "gcc"]])

    def test_again_runs_the_last_command_again(self):
        self.press(self.app(), ":", *typed("build ship"), "enter", "a")
        self.assertEqual(self.ran, [["build", "ship", "--compiler", "gcc"]] * 2)

    def test_q_quits(self):
        app = self.press(self.app(), "q")
        self.assertEqual(app.views, [])

    def test_the_cursor_scrolls_into_view(self):
        for index in range(MANY_RECIPES):
            self.context.store.save_recipe(f"recipe-{index:02d}", ["build", "dev", "--compiler", "gcc"])
        app = self.press(self.app(SMALL_SIZE), "tab", "G")
        self.assertIn(f"recipe-{MANY_RECIPES - 1:02d}", self.screen(app))

    def test_a_narrow_terminal_still_shows_every_pane(self):
        text = self.screen(self.press(self.app(NARROW_SIZE)))
        for title in ("Recipes", "Most used", "Builds"):
            self.assertIn(title, text)

    def test_the_terminal_is_given_back_when_something_fails(self):
        def failing(request):
            raise RuntimeError("boom")
        app = App(self.context, FakeTerminal(SCREEN_SIZE), executor=failing)
        app.terminal.keys.extend([":", *typed("build"), " ", "d", "e", "v", "enter"])
        with self.assertRaises(RuntimeError):
            app.loop()
        self.assertEqual(app.terminal.depth, 0)

    def test_an_interrupted_command_comes_back_to_the_menu(self):
        app = self.app()
        request = parse(["build", "dev"], self.context.project)
        with mock.patch.object(core, "execute", side_effect=KeyboardInterrupt), \
                mock.patch("builtins.input", return_value=""), \
                contextlib.redirect_stdout(io.StringIO()):
            code, _ = app.run_in_terminal(request)
        self.assertEqual(code, INTERRUPTED_CODE)
        self.assertEqual(app.terminal.suspensions, 1)


@unittest.skipUnless(HAVE_PTY and os.name == "posix", "needs a pseudo-terminal")
class PosixTerminalTest(unittest.TestCase):
    def setUp(self):
        import termios
        from assisi.tui.term import PosixTerminal
        self.termios = termios
        self.master, self.slave = os.openpty()
        self.output = os.fdopen(os.dup(self.slave), "w", encoding="utf-8")
        self.terminal = PosixTerminal(self.slave, self.output)

    def tearDown(self):
        self.output.close()
        os.close(self.master)
        os.close(self.slave)

    def written(self) -> bytes:
        os.set_blocking(self.master, False)
        try:
            return os.read(self.master, 1 << 16)
        except BlockingIOError:
            return b""

    def test_the_mode_comes_back_after_an_exception(self):
        before = self.termios.tcgetattr(self.slave)
        with self.assertRaises(RuntimeError):
            with self.terminal.session():
                inside = self.termios.tcgetattr(self.slave)
                raise RuntimeError("boom")
        self.assertEqual(self.termios.tcgetattr(self.slave), before)
        self.assertNotEqual(inside, before)
        self.assertTrue(self.written().endswith(b"\x1b[?1049l"))

    def test_ctrl_c_still_interrupts_inside_a_session(self):
        with self.terminal.session():
            local_flags = self.termios.tcgetattr(self.slave)[3]
        self.assertTrue(local_flags & self.termios.ISIG)
        self.assertFalse(local_flags & self.termios.ICANON)

    def test_keys_are_read_whole_and_a_lone_escape_is_escape(self):
        with self.terminal.session():
            os.write(self.master, b"\x1b[A")
            self.assertEqual(self.terminal.read_key(1.0), "up")
            os.write(self.master, b"\x1b")
            self.assertEqual(self.terminal.read_key(1.0), "esc")
            self.assertIsNone(self.terminal.read_key(0.0))


if __name__ == "__main__":
    unittest.main()
