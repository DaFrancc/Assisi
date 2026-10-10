"""What the TUI shows and what each key asks for, without Textual.

Every request the TUI runs is made here, through request.parse, so the TUI can
only ask for what a command line could have said, and these rules are tested
on any Python without the TUI's environment.
"""

from __future__ import annotations

import shlex
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence, Tuple

from assisi import request as requests
from assisi.options import Kind, Option
from assisi.process import describe
from assisi.project import BuildSpec, BuildStatus, Project, Status, UsageError, spec_of
from assisi.store import Entry, Store, rank_common

STATUS_GLYPH = {Status.BUILT: "●", Status.STALE: "◐", Status.MISSING: "○"}

# The columns of the builds table, after the build's name, and the field of
# BuildStatus each one shows.
STATUS_COLUMNS = (("Configured", "configured"), ("Editor", "editor"), ("Game", "game"),
                  ("Cooked", "cooked"), ("Packed", "packed"))

# How many history entries the home screen lists; each has a number key.
FREQUENT_LIMIT = 9

# What each key does to the build under the cursor: the command and the words
# that go before and after the build's name on its command line.
BUILD_KEYS: Dict[str, Tuple[str, Tuple[str, ...], Tuple[str, ...]]] = {
    "b": ("build", (), ()),
    "g": ("build", (), ("--game",)),
    "k": ("cook", (), ()),
    "p": ("package", (), ()),
    "t": ("test", (), ()),
    "e": ("run", ("editor",), ()),
    "r": ("run", ("game",), ()),
}

SECONDS_PER_MINUTE = 60


def command_line(argv: Sequence[str]) -> str:
    return "./assisi " + describe(argv)


def format_duration(seconds: float) -> str:
    whole = int(round(seconds))
    minutes, rest = divmod(whole, SECONDS_PER_MINUTE)
    return f"{minutes}m{rest:02d}s" if minutes else f"{rest}s"


def matches(text: str, needle: str) -> bool:
    return needle.strip().lower() in text.lower()


@dataclass(frozen=True)
class BuildRow:
    tree: str
    spec: BuildSpec
    status: BuildStatus
    is_default: bool

    def glyphs(self) -> List[Tuple[str, Status]]:
        return [(STATUS_GLYPH[getattr(self.status, field)], getattr(self.status, field))
                for _, field in STATUS_COLUMNS]

    def words(self) -> str:
        return f"{self.spec.level} {self.spec.compiler} {self.spec.extras()}"


def build_rows(project: Project, needle: str = "") -> List[BuildRow]:
    """Every build, plain levels first, with the developer's default compiler's
    before the others."""
    default = project.default_build()
    preferred = project.default_compiler()
    trees = sorted(project.trees(), key=lambda tree: (spec_of(tree).extras() != "",
                                                     spec_of(tree).compiler != preferred))
    rows = [BuildRow(tree, spec_of(tree), project.build_status(tree), tree == default) for tree in trees]
    return [row for row in rows if matches(row.words(), needle)]


def frequent(store: Store, needle: str = "") -> List[Entry]:
    """The most used requests, most used first, for the home screen."""
    return [entry for entry in rank_common(store.history())
            if matches(" ".join(entry.argv), needle)][:FREQUENT_LIMIT]


def recipes(store: Store, needle: str = "") -> List[Tuple[str, List[str]]]:
    return [(name, argv) for name, argv in store.recipes().items()
            if matches(name + " " + " ".join(argv), needle)]


def request_for_build_key(key: str, tree: str, project: Project) -> requests.Request:
    """The request a key on a build row makes; UsageError when it does not
    apply to that build, such as cooking a sanitizer build."""
    command, before, after = BUILD_KEYS[key]
    return requests.parse([command, *before, *spec_of(tree).argv(), *after], project)


def form_options(module) -> List[Option]:
    """The options a command's form shows, in declaration order."""
    return [option for option in module.OPTIONS if not option.hidden]


def value_choices(option: Option, project: Project) -> List[str]:
    """What a list in the form offers: only the compilers this host builds with."""
    if option.name == "compiler":
        return list(project.compilers())
    return list(option.choices)


class FormModel:
    """A command's options as the form holds them, and the request they make."""

    def __init__(self, module, project: Project, initial: Optional[requests.Request] = None):
        self.module = module
        self.project = project
        self.values: Dict[str, object] = {}
        for option in form_options(module):
            self.values[option.name] = self._initial(option, initial)

    def _initial(self, option: Option, initial: Optional[requests.Request]) -> object:
        given = initial.get(option.name) if initial is not None else None
        if option.kind is Kind.FLAG:
            return bool(given)
        if option.kind is Kind.LEVELS:
            # The form picks one level; the command line takes several.
            if given:
                return given[0]
            return self.project.default_level() if option.defaulted else None
        if option.kind is Kind.LEVEL:
            return given or (self.project.default_level() if option.defaulted or option.required else None)
        if option.kind is Kind.VALUE:
            return given or (self.project.default_compiler() if option.name == "compiler" else None)
        if option.kind is Kind.CHOICE:
            return given or option.choices[0]
        if option.kind is Kind.PASSTHROUGH:
            return describe(given) if given else ""
        if option.kind is Kind.NUMBER:
            return str(given if given is not None else option.default)
        return given or ""

    def set(self, name: str, value: object) -> None:
        self.values[name] = value

    def argv(self) -> List[str]:
        argv = [self.module.NAME]
        passthrough: List[str] = []
        for option in form_options(self.module):
            value = self.values[option.name]
            if option.kind is Kind.FLAG:
                if value:
                    argv.append(option.flag)
            elif option.kind is Kind.PASSTHROUGH:
                passthrough = shlex.split(str(value)) if value else []
            elif option.kind is Kind.VALUE:
                if value not in (None, ""):
                    argv += [option.flag, str(value)]
            elif value not in (None, ""):
                argv.append(str(value))
        if passthrough:
            argv += [requests.PASSTHROUGH_MARK, *passthrough]
        return argv

    def request(self) -> requests.Request:
        return requests.parse(self.argv(), self.project)

    def preview(self) -> Tuple[bool, str]:
        """The command line this form would run, or why it cannot run."""
        try:
            return True, command_line(self.request().to_argv())
        except (UsageError, ValueError) as error:
            return False, str(error)
