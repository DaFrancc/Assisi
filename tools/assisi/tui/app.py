"""The TUI: pick a build or a saved command and run it, from the keyboard.

The home screen holds the developer's recipes, their most used commands and
every build with what exists of it. Single keys act, as in vim: they move,
open a command's form or run something. Typing happens only where something
has to be typed: a form's text field, the filter, a recipe's name and the
command line opened with ':'. The status line always shows the command line
whatever is under the cursor would run, so the TUI teaches the command line as
it goes.

Running steps out of the TUI and back: a build's output goes straight to the
terminal, keeping ninja's progress line and the compiler's colour.

Every request comes from tui/model.py, which only makes what request.parse
accepts — nothing here can run what the command line could not.
"""

from __future__ import annotations

import functools
import time
from typing import Callable, List, Optional, Tuple

from assisi import core
from assisi.commands.save import name_problem
from assisi.options import Kind, Option
from assisi.project import COMPILER_KEY, LEVEL_KEY, Status, UsageError, spec_of
from assisi.request import Request, parse
from assisi.store import Entry
from assisi.tui import model
from assisi.tui.canvas import PLAIN, Canvas, Color, Style, render

# The result of running a request: its exit code and how long it took.
Outcome = Tuple[int, float]
Executor = Callable[[Request], Outcome]

# What a run stopped with Ctrl+C exits with, as a shell reports it: 128 plus
# the number of SIGINT.
INTERRUPTED_CODE = 130

# How long the loop waits for a key before drawing again, which is how a
# resized terminal is noticed without a signal Windows does not have.
POLL_INTERVAL_S = 0.25

# The narrowest terminal that has the recipes and most used beside the builds
# rather than above them.
SPLIT_WIDTH = 80

# The share of the width the recipes and most used column takes: a third.
LEFT_SHARE = 3

# The rows the frame keeps: the title above the panes, and the status line and
# message line below them.
TITLE_ROWS = 1
STATUS_FROM_BOTTOM = 2
MESSAGE_FROM_BOTTOM = 1

# The panes of the home screen, in the order Tab visits them.
RECIPES, FREQUENT, BUILDS = range(3)
PANE_TITLES = ("Recipes", "Most used", "Builds")

# The builds table's columns: the width of the level and compiler columns, and
# of each status column, whose header is its title cut to that width.
LEVEL_WIDTH = 7
COMPILER_WIDTH = 10
STATUS_WIDTH = 5

# The form's columns: where the value starts after the option's name, and
# where the help starts after the value.
FORM_INDENT = 2
FORM_NAME_WIDTH = 20
FORM_VALUE_WIDTH = 18

# Shown for a choice left blank.
BLANK_CHOICE = "none"

# The keys that run a most used command, by its number.
NUMBER_KEYS = tuple(str(number) for number in range(1, model.FREQUENT_LIMIT + 1))

ACCENT = Style(color=Color.CYAN, bold=True)
BOLD = Style(bold=True)
DIM = Style(dim=True)
ERROR = Style(color=Color.RED, bold=True)
SUCCESS = Style(color=Color.GREEN)
WARNING = Style(color=Color.YELLOW, bold=True)
SELECTED = Style(reverse=True)
STATUS_LINE = Style(reverse=True)
STATUS_LINE_ERROR = Style(color=Color.RED, reverse=True)
STATUS_STYLES = {Status.BUILT: SUCCESS, Status.STALE: Style(color=Color.YELLOW), Status.MISSING: DIM}

# Every key of the home screen, for the help overlay.
HOME_KEYS = (
    ("j k  ↑ ↓", "move"),
    ("g G", "first, last"),
    ("Tab  h l", "next or previous pane"),
    ("Enter", "run a recipe or command; on a build, open its build menu"),
    ("b c p v", "on a build: the build, cook, package or release menu"),
    ("e r t", "on a build: the menu to start its editor, start its game, or test it"),
    ("D", "on a build: make its level and compiler your defaults"),
    ("1-9", "run that most used command"),
    ("a", "run the last command again"),
    ("s", "save the selected command, or the one that just ran, as a recipe"),
    ("d", "on a recipe: delete it"),
    (":", "type a command; Tab completes it"),
    ("/", "filter the lists"),
    ("Esc", "clear the filter and the last result"),
    ("q", "quit"),
)

HOME_HINT = "Enter run · b build · p package · e editor · : command · / filter · ? keys · q quit"
FORM_HINT = "j k move · Space h l change · i type · Ctrl+R run · Ctrl+S save · Esc back"


def bottom_row(canvas: Canvas, from_bottom: int) -> int:
    return canvas.height - from_bottom


def draw_status(canvas: Canvas, text: str, style: Style) -> None:
    """The status line: @p text across the whole width, in reverse video."""
    row = bottom_row(canvas, STATUS_FROM_BOTTOM)
    canvas.put(0, row, f" {text}".ljust(canvas.width), style)


def draw_message(canvas: Canvas, text: str, style: Style) -> None:
    canvas.put(0, bottom_row(canvas, MESSAGE_FROM_BOTTOM), text.ljust(canvas.width), style)


class LineEdit:
    """One line of text being typed, with a cursor."""

    def __init__(self, text: str = ""):
        self.text = text
        self.cursor = len(text)

    def key(self, name: str) -> bool:
        """Apply @p name; False when it is not an editing key."""
        if len(name) == 1 and name.isprintable():
            self.text = self.text[:self.cursor] + name + self.text[self.cursor:]
            self.cursor += 1
        elif name == "backspace":
            if self.cursor > 0:
                self.text = self.text[:self.cursor - 1] + self.text[self.cursor:]
                self.cursor -= 1
        elif name == "delete":
            self.text = self.text[:self.cursor] + self.text[self.cursor + 1:]
        elif name == "left":
            self.cursor = max(0, self.cursor - 1)
        elif name == "right":
            self.cursor = min(len(self.text), self.cursor + 1)
        elif name in ("home", "ctrl+a"):
            self.cursor = 0
        elif name in ("end", "ctrl+e"):
            self.cursor = len(self.text)
        elif name == "ctrl+u":
            self.text = self.text[self.cursor:]
            self.cursor = 0
        else:
            return False
        return True

    def draw(self, canvas: Canvas, position: Tuple[int, int], width: int) -> int:
        """Draw the text in @p width columns, scrolled to keep the cursor in
        them, with the cursor's cell reversed; returns the columns used."""
        x, y = position
        start = max(0, self.cursor - width + 1)
        shown = self.text[start:start + width]
        canvas.put(x, y, shown)
        cursor_x = x + self.cursor - start
        canvas.put(cursor_x, y, (self.text[self.cursor:] or " ")[0], SELECTED)
        return max(len(shown), cursor_x - x + 1)


class ListPane:
    """A titled list with a cursor, scrolled to keep the cursor in view."""

    def __init__(self, title: str):
        self.title = title
        self.cursor = 0
        self.top = 0

    def move(self, delta: int, count: int) -> None:
        self.cursor = max(0, min(count - 1, self.cursor + delta))

    def clamp(self, count: int) -> None:
        self.move(0, count)

    def visible(self, count: int, rows: int) -> range:
        """The items to draw in @p rows rows, the cursor among them."""
        if self.cursor < self.top:
            self.top = self.cursor
        elif self.cursor >= self.top + rows:
            self.top = self.cursor - rows + 1
        self.top = max(0, min(self.top, max(0, count - rows)))
        return range(self.top, min(count, self.top + rows))


class View:
    """One layer of the screen. The app draws every view, bottom first, and
    hands keys to the top one."""

    def __init__(self, app: "App"):
        self.app = app

    def draw(self, canvas: Canvas) -> None:
        raise NotImplementedError

    def key(self, name: str) -> None:
        raise NotImplementedError


class Home(View):
    def __init__(self, app: "App"):
        super().__init__(app)
        self.panes = [ListPane(title) for title in PANE_TITLES]
        self.focus = BUILDS
        self.needle = ""
        self.recipes: List[Tuple[str, List[str]]] = []
        self.frequent: List[Entry] = []
        self.rows: List[model.BuildRow] = []
        self.refresh()
        defaults = [index for index, row in enumerate(self.rows) if row.is_default]
        self.panes[BUILDS].cursor = defaults[0] if defaults else 0

    def counts(self) -> Tuple[int, int, int]:
        return len(self.recipes), len(self.frequent), len(self.rows)

    def refresh(self) -> None:
        context = self.app.context
        self.recipes = model.recipes(context.store, self.needle)
        self.frequent = model.frequent(context.store, self.needle)
        self.rows = model.build_rows(context.project, self.needle)
        for pane, count in zip(self.panes, self.counts()):
            pane.clamp(count)

    def set_needle(self, needle: str) -> None:
        self.needle = needle
        self.refresh()

    # --- What is under the cursor

    def current(self) -> Optional[int]:
        """The focused pane's cursor, or None when that pane is empty."""
        if self.counts()[self.focus] == 0:
            return None
        return self.panes[self.focus].cursor

    def current_tree(self) -> Optional[str]:
        index = self.current()
        return self.rows[index].tree if self.focus == BUILDS and index is not None else None

    def selected_argv(self) -> Optional[List[str]]:
        index = self.current()
        if index is None:
            return None
        if self.focus == RECIPES:
            return self.recipes[index][1]
        if self.focus == FREQUENT:
            return self.frequent[index].argv
        return ["build", *spec_of(self.rows[index].tree).argv()]

    # --- Keys

    def key(self, name: str) -> None:
        if self.move(name) or self.global_key(name):
            return
        tree = self.current_tree()
        if tree is not None and name in model.BUILD_MENUS:
            self.app.open_build_menu(name, tree)
        elif tree is not None and name == "D":
            self.app.make_default(tree)
        elif self.focus == RECIPES and name == "d" and self.current() is not None:
            self.app.ask_to_forget(self.recipes[self.current()][0])

    def move(self, name: str) -> bool:
        pane = self.panes[self.focus]
        count = self.counts()[self.focus]
        steps = {"j": 1, "down": 1, "k": -1, "up": -1, "g": -count, "home": -count, "G": count, "end": count}
        if name in steps:
            pane.move(steps[name], count)
        elif name in ("tab", "l", "right"):
            self.focus = (self.focus + 1) % len(self.panes)
        elif name in ("shift+tab", "h", "left"):
            self.focus = (self.focus - 1) % len(self.panes)
        else:
            return False
        return True

    def global_key(self, name: str) -> bool:
        app = self.app
        if name == "q":
            app.close(self)
        elif name == "enter":
            self.activate()
        elif name in NUMBER_KEYS:
            app.run_frequent(int(name) - 1)
        elif name == "a":
            app.again()
        elif name == "s":
            argv = app.last.to_argv() if app.last is not None and app.result is not None else self.selected_argv()
            if argv:
                app.push(SaveLine(app, argv))
        elif name == ":":
            app.push(CommandLine(app))
        elif name == "/":
            app.push(FilterLine(app, self.needle))
        elif name == "?":
            app.push(Help(app, HOME_KEYS))
        elif name == "esc":
            app.result = None
            self.set_needle("")
        else:
            return False
        return True

    def activate(self) -> None:
        tree = self.current_tree()
        if tree is not None:
            self.app.open_build_menu("b", tree)
            return
        argv = self.selected_argv()
        if argv:
            self.app.run_argv(argv)

    # --- Drawing

    def draw(self, canvas: Canvas) -> None:
        project = self.app.context.project
        canvas.put(0, 0, " Assisi", BOLD)
        canvas.put(len(" Assisi  "), 0, f"default: {project.default_level()} with {project.default_compiler()}", DIM)
        if self.needle:
            label = f"filter: {self.needle} "
            canvas.put(canvas.width - len(label), 0, label, WARNING)
        for index, (position, size) in enumerate(self.layout(canvas)):
            self.draw_pane(canvas, index, position, size)
        argv = self.selected_argv()
        draw_status(canvas, model.command_line(argv) if argv else "", STATUS_LINE)
        self.app.draw_message(canvas, HOME_HINT)

    def layout(self, canvas: Canvas) -> List[Tuple[Tuple[int, int], Tuple[int, int]]]:
        """Where each pane goes, as (x, y) and (width, height), in pane order."""
        top = TITLE_ROWS
        height = canvas.height - TITLE_ROWS - STATUS_FROM_BOTTOM
        width = canvas.width
        if width >= SPLIT_WIDTH:
            left = width // LEFT_SHARE
            upper = height // 2
            return [((0, top), (left, upper)), ((0, top + upper), (left, height - upper)),
                    ((left, top), (width - left, height))]
        third = height // len(self.panes)
        return [((0, top), (width, third)), ((0, top + third), (width, third)),
                ((0, top + 2 * third), (width, height - 2 * third))]

    def draw_pane(self, canvas: Canvas, index: int, position: Tuple[int, int], size: Tuple[int, int]) -> None:
        x, y = position
        width, height = size
        focused = index == self.focus
        canvas.box(x, y, size, self.panes[index].title, ACCENT if focused else DIM)
        inner_x, inner_y, inner_width = x + 1, y + 1, width - 2
        rows = height - 2
        if index == BUILDS:
            self.draw_build_header(canvas, (inner_x, inner_y))
            inner_y += 1
            rows -= 1
        count = self.counts()[index]
        pane = self.panes[index]
        for row, item in enumerate(pane.visible(count, max(0, rows))):
            line_y = inner_y + row
            self.draw_item(canvas, index, item, (inner_x, line_y))
            if focused and item == pane.cursor:
                canvas.fill(inner_x, line_y, inner_width, SELECTED)

    def draw_item(self, canvas: Canvas, pane: int, item: int, position: Tuple[int, int]) -> None:
        x, y = position
        if pane == RECIPES:
            name, argv = self.recipes[item]
            used = canvas.put(x + 1, y, name, BOLD)
            canvas.put(x + used + 3, y, model.command_line(argv), DIM)
        elif pane == FREQUENT:
            self.draw_frequent(canvas, item, position)
        else:
            self.draw_build(canvas, self.rows[item], position)

    def draw_frequent(self, canvas: Canvas, item: int, position: Tuple[int, int]) -> None:
        x, y = position
        entry = self.frequent[item]
        x += 1 + canvas.put(x + 1, y, f"{item + 1} ", BOLD)
        x += canvas.put(x, y, "✔ " if entry.last_ok else "✘ ", SUCCESS if entry.last_ok else ERROR)
        x += canvas.put(x, y, model.command_line(entry.argv))
        canvas.put(x, y, f"  ×{entry.count} · {model.format_duration(entry.duration)}", DIM)

    def draw_build_header(self, canvas: Canvas, position: Tuple[int, int]) -> None:
        x, y = position
        x += 1
        for title, width in (("Level", LEVEL_WIDTH), ("Compiler", COMPILER_WIDTH)):
            canvas.put(x, y, title, DIM)
            x += width
        for title, _ in model.STATUS_COLUMNS:
            canvas.put(x, y, title[:STATUS_WIDTH - 1], DIM)
            x += STATUS_WIDTH
        canvas.put(x, y, "Extras", DIM)

    def draw_build(self, canvas: Canvas, row: model.BuildRow, position: Tuple[int, int]) -> None:
        x, y = position
        x += 1
        style = ACCENT if row.is_default else PLAIN
        canvas.put(x, y, row.spec.level, style)
        canvas.put(x + LEVEL_WIDTH, y, row.spec.compiler, style)
        x += LEVEL_WIDTH + COMPILER_WIDTH
        for glyph, status in row.glyphs():
            canvas.put(x + 1, y, glyph, STATUS_STYLES[status])
            x += STATUS_WIDTH
        canvas.put(x, y, row.spec.tags(), DIM)


class Form(View):
    """Every option of one command, made from its declarations."""

    def __init__(self, app: "App", form: model.FormModel):
        super().__init__(app)
        self.form = form
        self.options = model.form_options(form.module)
        self.cursor = 0
        self.editing: Optional[LineEdit] = None
        # A choice offers "none" only where leaving it out means something:
        # no sanitizer, or clean naming no level.
        self.blank = {option.name for option in self.options if form.values[option.name] is None}

    def option(self) -> Option:
        return self.options[self.cursor]

    def choices(self, option: Option) -> List[Optional[str]]:
        blank: List[Optional[str]] = [None] if option.name in self.blank else []
        return blank + model.value_choices(option, self.form.project)

    def is_choice(self, option: Option) -> bool:
        return option.kind in (Kind.LEVEL, Kind.LEVELS, Kind.VALUE, Kind.CHOICE)

    def change(self, step: int) -> None:
        """Toggle the flag under the cursor, or step its choice by @p step."""
        option = self.option()
        value = self.form.values[option.name]
        if option.kind is Kind.FLAG:
            self.form.set(option.name, not value)
        elif self.is_choice(option):
            choices = self.choices(option)
            index = choices.index(value) if value in choices else -1
            self.form.set(option.name, choices[(index + step) % len(choices)])

    def key(self, name: str) -> None:
        if self.editing is not None:
            self.edit(name)
            return
        option = self.option()
        if name in ("j", "down", "k", "up"):
            step = 1 if name in ("j", "down") else -1
            self.cursor = max(0, min(len(self.options) - 1, self.cursor + step))
        elif name in ("l", "right", "h", "left") and option.kind is not Kind.FLAG:
            self.change(1 if name in ("l", "right") else -1)
        elif name in (" ", "enter", "l", "right", "h", "left", "i", "a"):
            self.activate(name)
        elif name == "ctrl+r":
            self.run()
        elif name == "ctrl+s":
            self.save()
        elif name in ("esc", "q"):
            self.app.close(self)

    def activate(self, name: str) -> None:
        """Space, Enter and the editing keys on the field under the cursor."""
        option = self.option()
        if option.kind is Kind.FLAG or self.is_choice(option):
            if name not in ("i", "a"):
                self.change(1)
        else:
            self.editing = LineEdit(str(self.form.values[option.name] or ""))

    def edit(self, name: str) -> None:
        if name in ("esc", "enter"):
            self.editing = None
            return
        if self.editing.key(name):
            self.form.set(self.option().name, self.editing.text)

    def run(self) -> None:
        try:
            request = self.form.request()
        except (UsageError, ValueError) as error:
            self.app.error(str(error))
            return
        self.app.close(self)
        self.app.run_request(request)

    def save(self) -> None:
        try:
            argv = self.form.request().to_argv()
        except (UsageError, ValueError) as error:
            self.app.error(str(error))
            return
        self.app.push(SaveLine(self.app, argv))

    def value_text(self, option: Option) -> str:
        value = self.form.values[option.name]
        if option.kind is Kind.FLAG:
            return "[x]" if value else "[ ]"
        if self.is_choice(option):
            return f"‹ {value or BLANK_CHOICE} ›"
        return str(value or "")

    def draw(self, canvas: Canvas) -> None:
        module = self.form.module
        used = canvas.put(1, 0, module.NAME, BOLD)
        canvas.put(used + 3, 0, module.HELP, DIM)
        value_x = FORM_INDENT + FORM_NAME_WIDTH
        for index, option in enumerate(self.options):
            y = TITLE_ROWS + 1 + index
            canvas.put(FORM_INDENT, y, option.name.replace("_", "-"), BOLD)
            if index == self.cursor and self.editing is not None:
                self.editing.draw(canvas, (value_x, y), FORM_VALUE_WIDTH)
            else:
                canvas.put(value_x, y, self.value_text(option))
            canvas.put(value_x + FORM_VALUE_WIDTH + 1, y, option.help, DIM)
            if index == self.cursor and self.editing is None:
                canvas.fill(FORM_INDENT, y, FORM_NAME_WIDTH + FORM_VALUE_WIDTH, SELECTED)
        ok, text = self.form.preview()
        draw_status(canvas, text, STATUS_LINE if ok else STATUS_LINE_ERROR)
        self.app.draw_message(canvas, "-- TYPING --  Enter or Esc to stop" if self.editing else FORM_HINT)


class Line(View):
    """A line typed at the bottom of the screen, over the view below it."""

    PROMPT = ""

    def __init__(self, app: "App", text: str = ""):
        super().__init__(app)
        self.edit = LineEdit(text)

    def key(self, name: str) -> None:
        if name == "esc":
            self.app.close(self)
            self.cancel()
        elif name == "enter":
            self.submit(self.edit.text)
        elif self.edit.key(name):
            self.changed(self.edit.text)

    def prompt(self) -> str:
        return self.PROMPT

    def draw(self, canvas: Canvas) -> None:
        row = bottom_row(canvas, MESSAGE_FROM_BOTTOM)
        canvas.put(0, row, " " * canvas.width)
        used = canvas.put(0, row, self.prompt(), BOLD)
        used += self.edit.draw(canvas, (used, row), canvas.width - used - 1)
        self.draw_after(canvas, (used + 2, row))

    def draw_after(self, canvas: Canvas, position: Tuple[int, int]) -> None:
        """Anything a line shows after what has been typed."""

    def changed(self, text: str) -> None:
        """Called as the text changes."""

    def cancel(self) -> None:
        """Called after Esc has closed the line."""

    def submit(self, text: str) -> None:
        raise NotImplementedError


class FilterLine(Line):
    """'/': narrows every pane to what contains the text, as it is typed."""

    PROMPT = "/"

    def changed(self, text: str) -> None:
        self.app.home.set_needle(text)

    def cancel(self) -> None:
        self.app.home.set_needle("")

    def submit(self, text: str) -> None:
        self.app.close(self)


class SaveLine(Line):
    """Asks for a name and saves a command line under it."""

    def __init__(self, app: "App", argv: List[str]):
        super().__init__(app)
        self.argv = argv

    def prompt(self) -> str:
        return f"save {model.command_line(self.argv)} as: "

    def problem(self) -> str:
        return name_problem(self.edit.text, self.app.context.project) if self.edit.text else ""

    def draw_after(self, canvas: Canvas, position: Tuple[int, int]) -> None:
        canvas.put(position[0], position[1], self.problem(), ERROR)

    def submit(self, text: str) -> None:
        if not text or self.problem():
            return
        self.app.close(self)
        self.app.save_recipe(text, self.argv)


class CommandLine(Line):
    """':': any command, a recipe's name, or a command's name for its form."""

    PROMPT = ":"

    def __init__(self, app: "App"):
        super().__init__(app)
        self.candidates: List[str] = []

    def key(self, name: str) -> None:
        if name == "tab":
            context = self.app.context
            text, self.candidates = model.complete(self.edit.text, context.store, context.project)
            self.edit = LineEdit(text)
            return
        self.candidates = []
        super().key(name)

    def draw(self, canvas: Canvas) -> None:
        super().draw(canvas)
        if len(self.candidates) > 1:
            draw_status(canvas, "  ".join(self.candidates), STATUS_LINE)

    def submit(self, text: str) -> None:
        context = self.app.context
        self.app.close(self)
        try:
            asked = model.line_request(text, context.store, context.project)
        except UsageError as error:
            self.app.error(str(error))
            return
        if isinstance(asked, model.FormModel):
            self.app.push(Form(self.app, asked))
        else:
            self.app.run_request(asked)


class Confirm(View):
    """Yes or no before something that deletes."""

    def __init__(self, app: "App", question: str, action: Callable[[], None]):
        super().__init__(app)
        self.question = question
        self.action = action

    def key(self, name: str) -> None:
        if name == "y":
            self.app.close(self)
            self.action()
        elif name in ("n", "esc", "q"):
            self.app.close(self)

    def draw(self, canvas: Canvas) -> None:
        draw_message(canvas, f"{self.question} (y/n)", WARNING)


class Help(View):
    """Every key of the screen below, in a box over it; any key closes it."""

    def __init__(self, app: "App", entries: Tuple[Tuple[str, str], ...]):
        super().__init__(app)
        self.entries = entries

    def key(self, name: str) -> None:
        self.app.close(self)

    def draw(self, canvas: Canvas) -> None:
        keys_width = max(len(keys) for keys, _ in self.entries)
        lines = [f"{keys.ljust(keys_width)}   {what}" for keys, what in self.entries]
        width = min(canvas.width, max(len(line) for line in lines) + 4)
        height = min(canvas.height, len(lines) + 2)
        x = (canvas.width - width) // 2
        y = (canvas.height - height) // 2
        for row in range(y, y + height):
            canvas.put(x, row, " " * width)
        canvas.box(x, y, (width, height), "Keys", ACCENT)
        for index, line in enumerate(lines[:height - 2]):
            canvas.put(x + 2, y + 1 + index, line[:width - 4])


class App:
    def __init__(self, context: core.Context, terminal, executor: Optional[Executor] = None):
        self.context = context
        self.terminal = terminal
        self.executor = executor or self.run_in_terminal
        self.last: Optional[Request] = None
        # The last run's outcome, shown until the next key that clears it.
        self.result: Optional[Tuple[str, Style]] = None
        # A note for the developer, shown until the next key.
        self.message: Optional[Tuple[str, Style]] = None
        self.repaint = False
        self.home = Home(self)
        self.views: List[View] = [self.home]

    # --- The loop

    def loop(self) -> None:
        """Draw, wait for a key, act on it, until the last view is closed. The
        screen is redrawn whole after a run and on a new size, and otherwise
        only where it changed."""
        previous: Optional[Canvas] = None
        with self.terminal.session():
            while self.views:
                if self.repaint:
                    previous = None
                    self.repaint = False
                frame = self.frame()
                output = render(frame, previous)
                if output:
                    self.terminal.write(output)
                previous = frame
                name = self.terminal.read_key(POLL_INTERVAL_S)
                if name is not None:
                    self.dispatch(name)

    def frame(self) -> Canvas:
        width, height = self.terminal.size()
        canvas = Canvas(width, height)
        for view in self.views:
            view.draw(canvas)
        return canvas

    def dispatch(self, name: str) -> None:
        if name == "ctrl+c":
            raise KeyboardInterrupt
        self.message = None
        self.views[-1].key(name)

    # --- Views

    def push(self, view: View) -> None:
        self.views.append(view)

    def close(self, view: View) -> None:
        self.views.remove(view)

    def draw_message(self, canvas: Canvas, hint: str) -> None:
        """The message line: a note, else the last result, else @p hint."""
        text, style = self.message or self.result or (hint, DIM)
        draw_message(canvas, text, style)

    def error(self, text: str) -> None:
        self.message = (text, ERROR)

    def notify(self, text: str) -> None:
        self.message = (text, SUCCESS)

    # --- Running

    def run_in_terminal(self, request: Request) -> Outcome:
        """Leave the TUI, run @p request with the terminal attached, and come back
        once the developer has read the result. Ctrl+C stops the command, not
        the TUI."""
        with self.terminal.suspended():
            print(f"\n$ {model.command_line(request.to_argv())}", flush=True)
            started = time.monotonic()
            try:
                code = core.execute(request, self.context)
            except KeyboardInterrupt:
                code = INTERRUPTED_CODE
            elapsed = time.monotonic() - started
            verdict = "✔ done" if code == 0 else f"✘ exit {code}"
            print(f"\n{verdict} after {model.format_duration(elapsed)}")
            try:
                input("Press Enter to return to Assisi")
            except (KeyboardInterrupt, EOFError):
                print()
        return code, elapsed

    def run_request(self, request: Request) -> None:
        if self.is_destructive(request):
            question = f"Run {model.command_line(request.to_argv())}?"
            self.push(Confirm(self, question, functools.partial(self.execute, request)))
            return
        self.execute(request)

    @staticmethod
    def is_destructive(request: Request) -> bool:
        return request.command == "clean" or request.to_argv() == ["steamrt", "remove"]

    def execute(self, request: Request) -> None:
        code, elapsed = self.executor(request)
        self.last = request
        command = model.command_line(request.to_argv())
        duration = model.format_duration(elapsed)
        if code == 0:
            self.result = (f"✔ {command} · {duration}   (a again · s save · Esc hide)", SUCCESS)
        else:
            self.result = (f"✘ exit {code} · {command} · {duration}   (a again · s save · Esc hide)", ERROR)
        self.repaint = True
        self.home.refresh()

    def run_argv(self, argv: List[str]) -> None:
        try:
            self.run_request(parse(argv, self.context.project))
        except UsageError as error:
            self.error(str(error))

    def run_frequent(self, index: int) -> None:
        if index < len(self.home.frequent):
            self.run_argv(self.home.frequent[index].argv)

    def again(self) -> None:
        if self.last is not None:
            self.run_request(self.last)

    def open_build_menu(self, key: str, tree: str) -> None:
        try:
            form = model.form_for_build_key(key, tree, self.context.project)
        except UsageError as error:
            self.error(str(error))
            return
        self.push(Form(self, form))

    # --- Recipes and defaults

    def save_recipe(self, name: str, argv: List[str]) -> None:
        self.context.store.save_recipe(name, argv)
        self.notify(f"saved: ./assisi {name}")
        self.home.refresh()

    def ask_to_forget(self, name: str) -> None:
        self.push(Confirm(self, f"Delete the recipe {name}?", functools.partial(self.forget_recipe, name)))

    def forget_recipe(self, name: str) -> None:
        self.context.store.forget_recipe(name)
        self.home.refresh()

    def make_default(self, tree: str) -> None:
        """Make the row's level and compiler the defaults. A profiler or sanitizer
        build has no default form, so only its level and compiler are taken."""
        spec = spec_of(tree)
        settings = self.context.project.read_settings()
        settings[LEVEL_KEY] = spec.level
        settings[COMPILER_KEY] = spec.compiler
        self.context.project.write_settings(settings)
        self.notify(f"default: {spec.level} with {spec.compiler}")
        self.home.refresh()


def run(context: core.Context) -> int:
    """Open the TUI on this terminal. Ctrl+C anywhere but in a running command
    leaves it, with the terminal given back first."""
    from assisi.tui import term

    try:
        App(context, term.current()).loop()
    except KeyboardInterrupt:
        return INTERRUPTED_CODE
    return 0
