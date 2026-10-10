"""What a command accepts, declared as data.

The command line's parser, the TUI's form and a request's canonical command
line are all generated from these declarations, which is what keeps the two
front ends identical: the TUI can only offer what a flag can express, and
anything saved in history or a recipe can be typed back in.
"""

from __future__ import annotations

import enum
from dataclasses import dataclass
from typing import Tuple

from assisi.project import COMPILERS, CONFIGS, SANITIZERS


class Kind(enum.Enum):
    LEVEL = "level"  # one optimization level: debug, dev or ship
    LEVELS = "levels"  # any number of levels
    CHOICE = "choice"  # one word from a fixed list, as a positional
    VALUE = "value"  # --name word, the word from a fixed list
    STRING = "string"  # --name word, any word
    PATH = "path"  # --name path, to a file that has to exist
    FLAG = "flag"  # --name, on or off
    TEXT = "text"  # a free word, such as a recipe name
    NUMBER = "number"  # a positive whole number
    PASSTHROUGH = "passthrough"  # everything after --, handed to another program


@dataclass(frozen=True)
class Option:
    name: str
    kind: Kind
    help: str
    required: bool = False
    choices: Tuple[str, ...] = ()
    # NUMBER's value when none is given.
    default: int = 1
    # Whether a missing level or compiler takes the developer's default. Off
    # where leaving it out means something else, as for clean --all.
    defaulted: bool = True
    # Machinery for the tool's own use, kept out of help and the TUI's forms.
    hidden: bool = False

    @property
    def flag(self) -> str:
        return "--" + self.name.replace("_", "-")

    @property
    def is_positional(self) -> bool:
        return self.kind in (Kind.LEVEL, Kind.LEVELS, Kind.CHOICE, Kind.TEXT, Kind.NUMBER)


# The options that pick one build tree, shared by every command that builds:
# the level, then what it is built with and what is compiled in. The names are
# what build_tree reads.
COMPILER = "compiler"
PROFILER = "profiler"
SANITIZE = "sanitize"
STEAM_RUNTIME = "steam_runtime"


def build_options(levels: bool = False, sanitizers: bool = True, required: bool = False,
                  level_defaulted: bool = True) -> Tuple[Option, ...]:
    """@p levels takes several levels instead of one; @p sanitizers is off for
    commands that make a game, which a sanitizer build has none of; without
    @p level_defaulted, leaving the level out names no build at all."""
    if levels:
        level = Option("levels", Kind.LEVELS, "debug, dev or ship; any number"
                       + (" (default: your default level)" if level_defaulted else ""),
                       choices=CONFIGS, defaulted=level_defaulted)
    else:
        level = Option("level", Kind.LEVEL,
                       "debug, dev or ship" + ("" if required else " (default: your default level)"),
                       choices=CONFIGS, required=required, defaulted=level_defaulted)
    options = [
        level,
        Option(COMPILER, Kind.VALUE, "the compiler (default: your default compiler)", choices=COMPILERS),
        Option(PROFILER, Kind.FLAG, "compile in the Chiara profiler"),
    ]
    if sanitizers:
        options.append(Option(SANITIZE, Kind.VALUE, "a sanitizer build, debug level only: address or thread",
                              choices=SANITIZERS))
    options.append(Option(STEAM_RUNTIME, Kind.FLAG,
                          "build inside the Steam Runtime container, so the game runs on any distribution "
                          "from 2020 on (gcc ship only)"))
    return tuple(options)


PREVIOUS = "previous"
FRESH = "fresh"


def previous_options() -> Tuple[Option, ...]:
    """What a pak is laid out against. Packing keeps every asset that a previous
    package had where that package had it, so an update players download is the
    assets that changed rather than the whole file reshuffled."""
    return (
        Option(PREVIOUS, Kind.PATH, "lay the pak out against this earlier one (default: the newest release "
                                    "of the same level, if there is one)"),
        Option(FRESH, Kind.FLAG, "lay the pak out from scratch, ignoring every release"),
    )
