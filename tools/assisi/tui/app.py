"""The TUI: pick a build or a saved command and run it.

The home screen holds the developer's recipes, their most used commands and
every build with what exists of it. Whatever is under the cursor shows as the
command line it would run, so the TUI teaches the command line as it goes.
Running steps out of the TUI and back: a build's output goes straight to the
terminal, keeping ninja's progress line and the compiler's colour, which a
widget fed through a pipe would lose.

Every request comes from tui/model.py, which only makes what request.parse
accepts — nothing here can run what the command line could not.
"""

from __future__ import annotations

import time
from typing import Callable, List, Optional, Tuple

from rich.text import Text
from textual import on
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.command import DiscoveryHit, Hit, Hits, Provider
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.screen import ModalScreen, Screen
from textual.widgets import Button, DataTable, Footer, Header, Input, Label, OptionList, Select, Static, Switch
from textual.widgets.option_list import Option as ListOption

from assisi import core
from assisi.commands import by_name, catalog
from assisi.commands.save import name_problem
from assisi.options import Kind
from assisi.project import COMPILER_KEY, LEVEL_KEY, Project, Status, UsageError, spec_of
from assisi.request import Request, parse
from assisi.tui import model

# The home screen's lists of commands: recipes, and the most used.
HOME_LISTS = ("recipes", "frequent")

# The app's actions that read or change the home screen's panes.
HOME_ACTIONS = {"filter", "save", "again", "forget", "run_frequent", "dismiss_banner"}

# The result of running a request: its exit code and how long it took.
Outcome = Tuple[int, float]
Executor = Callable[[Request], Outcome]


def status_style(app: App, status: Status) -> str:
    theme = app.current_theme
    return {Status.BUILT: theme.success, Status.STALE: theme.warning}.get(status, "") or "dim"


class BuildTable(DataTable):
    """The builds, one row each. Its keys act on the build under the cursor."""

    BINDINGS = [
        Binding("b", "act('b')", "Build"),
        Binding("g", "act('g')", "Game", show=False),
        Binding("k", "act('k')", "Cook", show=False),
        Binding("p", "act('p')", "Package"),
        Binding("t", "act('t')", "Test", show=False),
        Binding("e", "act('e')", "Editor"),
        Binding("r", "act('r')", "Run game", show=False),
        Binding("o", "options", "Options"),
        Binding("D", "make_default", "Make default", show=False),
    ]

    def current_tree(self) -> Optional[str]:
        if self.row_count == 0:
            return None
        return str(self.coordinate_to_cell_key(self.cursor_coordinate).row_key.value)

    def action_act(self, key: str) -> None:
        tree = self.current_tree()
        if tree is not None:
            self.app.run_build_key(key, tree)

    def action_options(self) -> None:
        tree = self.current_tree()
        if tree is not None:
            self.app.open_form("build", model.request_for_build_key("b", tree, self.app.context.project))

    def action_make_default(self) -> None:
        tree = self.current_tree()
        if tree is not None:
            self.app.make_default(tree)


class SaveRecipeScreen(ModalScreen):
    """Asks for a name and saves a command line under it."""

    BINDINGS = [Binding("escape", "dismiss(None)", "Cancel")]

    def __init__(self, argv: List[str]):
        super().__init__()
        self.argv = argv

    def compose(self) -> ComposeResult:
        with Vertical(id="dialog"):
            yield Label("Save as a recipe", classes="dialog-title")
            yield Static(model.command_line(self.argv), classes="dialog-command")
            yield Input(placeholder="name, e.g. ship-it", id="recipe-name")
            yield Static("", id="recipe-problem")

    @on(Input.Changed, "#recipe-name")
    def check(self, event: Input.Changed) -> None:
        self.query_one("#recipe-problem", Static).update(name_problem(event.value, self.app.context.project)
                                                         if event.value else "")

    @on(Input.Submitted, "#recipe-name")
    def submit(self, event: Input.Submitted) -> None:
        if not name_problem(event.value, self.app.context.project):
            self.dismiss(event.value)


class ConfirmScreen(ModalScreen):
    """Yes or no before something that deletes."""

    BINDINGS = [Binding("escape", "dismiss(False)", "Cancel"), Binding("y", "dismiss(True)", "Yes")]

    def __init__(self, question: str):
        super().__init__()
        self.question = question

    def compose(self) -> ComposeResult:
        with Vertical(id="dialog"):
            yield Label(self.question, classes="dialog-title")
            with Horizontal(classes="dialog-buttons"):
                yield Button("Yes", variant="error", id="yes")
                yield Button("Cancel", id="no")

    @on(Button.Pressed)
    def answer(self, event: Button.Pressed) -> None:
        self.dismiss(event.button.id == "yes")


class FormScreen(Screen):
    """Every option of one command, made from its declarations."""

    BINDINGS = [
        Binding("escape", "app.pop_screen", "Back"),
        Binding("ctrl+r", "run", "Run"),
        Binding("ctrl+s", "save", "Save as recipe"),
    ]

    def __init__(self, module, initial: Optional[Request], project: Project):
        super().__init__()
        self.form = model.FormModel(module, project, initial)

    def compose(self) -> ComposeResult:
        yield Header()
        project = self.form.project
        with VerticalScroll(id="form"):
            yield Label(f"{self.form.module.NAME}: {self.form.module.HELP}", classes="form-title")
            for option in model.form_options(self.form.module):
                value = self.form.values[option.name]
                field_id = f"field-{option.name}"
                yield Label(option.help, classes="field-label")
                if option.kind in (Kind.LEVEL, Kind.LEVELS, Kind.VALUE, Kind.CHOICE):
                    # A blank entry only where leaving it out means something:
                    # no sanitizer, or clean naming no level.
                    choices = model.value_choices(option, project)
                    yield Select([(choice, choice) for choice in choices], value=value or Select.NULL,
                                 allow_blank=value is None, prompt="none", id=field_id)
                elif option.kind is Kind.FLAG:
                    yield Switch(value=bool(value), id=field_id)
                else:
                    yield Input(str(value), id=field_id)
            with Horizontal(classes="form-buttons"):
                yield Button("Run", variant="primary", id="run")
                yield Button("Save as recipe", id="save")
        yield Static("", id="command-bar")
        yield Footer()

    def on_mount(self) -> None:
        self.show_preview()

    def show_preview(self) -> None:
        ok, text = self.form.preview()
        bar = self.query_one("#command-bar", Static)
        bar.update(text)
        bar.set_class(not ok, "invalid")

    def changed(self, widget_id: Optional[str], value: object) -> None:
        if widget_id and widget_id.startswith("field-"):
            self.form.set(widget_id[len("field-"):], None if value is Select.NULL else value)
            self.show_preview()

    @on(Select.Changed)
    def select_changed(self, event: Select.Changed) -> None:
        self.changed(event.select.id, event.value)

    @on(Switch.Changed)
    def switch_changed(self, event: Switch.Changed) -> None:
        self.changed(event.switch.id, event.value)

    @on(Input.Changed)
    def input_changed(self, event: Input.Changed) -> None:
        self.changed(event.input.id, event.value)

    @on(Button.Pressed, "#run")
    def action_run(self) -> None:
        try:
            parsed = self.form.request()
        except (UsageError, ValueError) as error:
            self.notify(str(error), severity="error")
            return
        self.app.pop_screen()
        self.app.run_request(parsed)

    @on(Button.Pressed, "#save")
    def action_save(self) -> None:
        try:
            argv = self.form.request().to_argv()
        except (UsageError, ValueError) as error:
            self.notify(str(error), severity="error")
            return
        self.app.save_recipe(argv)


class CommandsProvider(Provider):
    """Every command, opening its form."""

    def entries(self) -> List[Tuple[str, str, object]]:
        return [(module.NAME, module.HELP, module) for module in catalog() if module.RECORDED]

    async def discover(self) -> Hits:
        for name, help_text, module in self.entries():
            yield DiscoveryHit(name, lambda module=module: self.app.open_form(module.NAME, None), help=help_text)

    async def search(self, query: str) -> Hits:
        matcher = self.matcher(query)
        for name, help_text, module in self.entries():
            score = matcher.match(name)
            if score > 0:
                yield Hit(score, matcher.highlight(name), lambda module=module: self.app.open_form(module.NAME, None),
                          help=help_text)


class RecipesProvider(Provider):
    """Every recipe, running it."""

    async def search(self, query: str) -> Hits:
        matcher = self.matcher(query)
        for name, argv in model.recipes(self.app.context.store):
            score = matcher.match(name)
            if score > 0:
                yield Hit(score, matcher.highlight(name), lambda argv=argv: self.app.run_argv(argv),
                          help=model.command_line(argv))


class AssisiApp(App):
    CSS_PATH = "assisi.tcss"
    TITLE = "Assisi"
    COMMANDS = App.COMMANDS | {CommandsProvider, RecipesProvider}

    BINDINGS = [
        Binding("q", "quit", "Quit"),
        Binding("slash", "filter", "Filter", key_display="/"),
        Binding("s", "save", "Save as recipe"),
        Binding("a", "again", "Again"),
        Binding("d", "forget", "Delete recipe", show=False),
        Binding("question_mark", "show_help_panel", "Keys", key_display="?"),
        Binding("escape", "dismiss_banner", "", show=False),
    ] + [Binding(str(number), f"run_frequent({number})", "", show=False)
         for number in range(1, model.FREQUENT_LIMIT + 1)]

    def __init__(self, context: core.Context, executor: Optional[Executor] = None):
        super().__init__()
        self.context = context
        self.executor = executor or self.run_in_terminal
        self.last: Optional[Request] = None
        self.needle = ""

    # --- Layout

    def compose(self) -> ComposeResult:
        yield Header()
        yield Input(placeholder="filter", id="filter", classes="hidden")
        with Horizontal(id="panes"):
            with Vertical(id="left"):
                yield OptionList(id="recipes")
                yield OptionList(id="frequent")
            yield BuildTable(id="builds", cursor_type="row", zebra_stripes=True)
        yield Static("", id="result-bar", classes="hidden")
        yield Static("", id="command-bar")
        yield Footer()

    def check_action(self, action: str, parameters: tuple) -> Optional[bool]:
        """Keys that act on the home screen do nothing while a form or dialog is open."""
        if action in HOME_ACTIONS and len(self.screen_stack) > 1:
            return False
        return True

    def on_mount(self) -> None:
        self.query_one("#recipes", OptionList).border_title = "Recipes"
        self.query_one("#frequent", OptionList).border_title = "Most used"
        table = self.query_one(BuildTable)
        table.border_title = "Builds"
        for title in ("Level", "Compiler", "Extras"):
            table.add_column(title, key=title.lower())
        for title, field in model.STATUS_COLUMNS:
            table.add_column(title, key=field)
        self.refresh_panes()
        table.focus()

    def refresh_panes(self) -> None:
        recipes = self.query_one("#recipes", OptionList)
        recipes.set_options([ListOption(Text.assemble((name, "bold"), "\n", (model.command_line(argv), "dim")),
                                        id=name)
                             for name, argv in model.recipes(self.context.store, self.needle)])
        frequent = self.query_one("#frequent", OptionList)
        frequent.set_options([self.frequent_option(number, entry) for number, entry
                              in enumerate(model.frequent(self.context.store, self.needle), start=1)])
        table = self.query_one(BuildTable)
        cursor = table.cursor_row
        table.clear()
        for row in model.build_rows(self.context.project, self.needle):
            style = "bold " + self.current_theme.accent if row.is_default else ""
            names = [Text(row.spec.level, style=style), Text(row.spec.compiler, style=style),
                     Text(row.spec.tags(), style="dim")]
            cells = [Text(glyph, style=status_style(self, status)) for glyph, status in row.glyphs()]
            table.add_row(*names, *cells, key=row.tree)
        if table.row_count:
            table.move_cursor(row=min(cursor, table.row_count - 1))
        project = self.context.project
        self.sub_title = f"default: {project.default_level()} with {project.default_compiler()}"

    def frequent_option(self, number: int, entry) -> ListOption:
        mark = ("✔", self.current_theme.success) if entry.last_ok else ("✘", self.current_theme.error)
        line = Text.assemble((f"{number} ", "bold"), mark, " ", model.command_line(entry.argv),
                             (f"  ×{entry.count} · {model.format_duration(entry.duration)}", "dim"))
        return ListOption(line, id=" ".join(entry.argv))

    def show_command(self, argv: Optional[List[str]]) -> None:
        self.query_one("#command-bar", Static).update(model.command_line(argv) if argv else "")

    # --- What is under the cursor

    def selected_argv(self) -> Optional[List[str]]:
        focused = self.focused
        # By id rather than type: a form's dropdown is an OptionList as well, and
        # its entries are option values, not commands.
        if isinstance(focused, OptionList) and focused.id in HOME_LISTS and focused.highlighted is not None:
            option = focused.get_option_at_index(focused.highlighted)
            if focused.id == "recipes":
                return self.context.store.recipes().get(option.id)
            return option.id.split(" ")
        if isinstance(focused, BuildTable):
            tree = focused.current_tree()
            return ["build", *spec_of(tree).argv()] if tree else None
        return None

    @on(OptionList.OptionHighlighted, "#recipes, #frequent")
    @on(DataTable.RowHighlighted, "BuildTable")
    def highlighted(self) -> None:
        self.show_command(self.selected_argv())

    def on_descendant_focus(self) -> None:
        # The home screen's command bar; a form keeps its own up to date.
        if len(self.screen_stack) == 1:
            self.show_command(self.selected_argv())

    @on(OptionList.OptionSelected, "#recipes, #frequent")
    def chosen(self) -> None:
        argv = self.selected_argv()
        if argv:
            self.run_argv(argv)

    @on(DataTable.RowSelected, "BuildTable")
    def row_chosen(self, event: DataTable.RowSelected) -> None:
        self.open_form("build", model.request_for_build_key("b", str(event.row_key.value), self.context.project))

    # --- Running

    def run_in_terminal(self, request: Request) -> Outcome:
        """Leave the TUI, run @p request with the terminal attached, and come back
        once the developer has read the result."""
        with self.suspend():
            print(f"\n$ {model.command_line(request.to_argv())}", flush=True)
            started = time.monotonic()
            code = core.execute(request, self.context)
            elapsed = time.monotonic() - started
            verdict = "✔ done" if code == 0 else f"✘ exit {code}"
            print(f"\n{verdict} after {model.format_duration(elapsed)}")
            input("Press Enter to return to Assisi")
        return code, elapsed

    def run_request(self, request: Request) -> None:
        if self.is_destructive(request):
            self.push_screen(ConfirmScreen(f"Run {model.command_line(request.to_argv())}?"),
                             callback=lambda yes: self.execute(request) if yes else None)
            return
        self.execute(request)

    @staticmethod
    def is_destructive(request: Request) -> bool:
        return request.command == "clean" or request.to_argv() == ["steamrt", "remove"]

    def execute(self, request: Request) -> None:
        code, elapsed = self.executor(request)
        self.last = request
        banner = self.query_one("#result-bar", Static)
        glyph = "✔" if code == 0 else f"✘ exit {code} ·"
        banner.update(f"{glyph} {model.command_line(request.to_argv())} · {model.format_duration(elapsed)}"
                      f"   (a again · s save · Esc hide)")
        banner.set_class(code == 0, "success")
        banner.set_class(code != 0, "failure")
        banner.remove_class("hidden")
        self.refresh_panes()

    def run_argv(self, argv: List[str]) -> None:
        try:
            self.run_request(parse(argv, self.context.project))
        except UsageError as error:
            self.notify(str(error), severity="error")

    def run_build_key(self, key: str, tree: str) -> None:
        try:
            self.run_request(model.request_for_build_key(key, tree, self.context.project))
        except UsageError as error:
            self.notify(str(error), severity="error")

    def open_form(self, command: str, initial: Optional[Request]) -> None:
        self.push_screen(FormScreen(by_name()[command], initial, self.context.project))

    def save_recipe(self, argv: List[str]) -> None:
        def saved(name: Optional[str]) -> None:
            if name:
                self.context.store.save_recipe(name, argv)
                self.notify(f"saved: ./assisi {name}")
                self.refresh_panes()
        self.push_screen(SaveRecipeScreen(argv), callback=saved)

    def make_default(self, tree: str) -> None:
        """Make the row's level and compiler the defaults. A profiler or sanitizer
        build has no default form, so only its level and compiler are taken."""
        spec = spec_of(tree)
        settings = self.context.project.read_settings()
        settings[LEVEL_KEY] = spec.level
        settings[COMPILER_KEY] = spec.compiler
        self.context.project.write_settings(settings)
        self.notify(f"default: {spec.level} with {spec.compiler}")
        self.refresh_panes()

    # --- Keys

    def action_run_frequent(self, number: int) -> None:
        entries = model.frequent(self.context.store, self.needle)
        if number <= len(entries):
            self.run_argv(entries[number - 1].argv)

    def action_again(self) -> None:
        if self.last is not None:
            self.run_request(self.last)

    def action_save(self) -> None:
        argv = self.last.to_argv() if self.last is not None and not self.query_one(
            "#result-bar").has_class("hidden") else self.selected_argv()
        if argv:
            self.save_recipe(argv)

    def action_forget(self) -> None:
        recipes = self.query_one("#recipes", OptionList)
        if self.focused is not recipes or recipes.highlighted is None:
            return
        name = recipes.get_option_at_index(recipes.highlighted).id

        def answered(yes: bool) -> None:
            if yes:
                self.context.store.forget_recipe(name)
                self.refresh_panes()
        self.push_screen(ConfirmScreen(f"Delete the recipe {name}?"), callback=answered)

    def action_filter(self) -> None:
        field = self.query_one("#filter", Input)
        field.remove_class("hidden")
        field.focus()

    @on(Input.Changed, "#filter")
    def filter_changed(self, event: Input.Changed) -> None:
        self.needle = event.value
        self.refresh_panes()

    @on(Input.Submitted, "#filter")
    def filter_done(self) -> None:
        self.query_one(BuildTable).focus()

    def action_dismiss_banner(self) -> None:
        field = self.query_one("#filter", Input)
        if self.focused is field:
            field.value = ""
            field.add_class("hidden")
            self.query_one(BuildTable).focus()
            return
        self.query_one("#result-bar").add_class("hidden")


def run(context: core.Context) -> int:
    AssisiApp(context).run()
    return 0
