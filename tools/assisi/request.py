"""A request: one command with its options filled in, and its canonical form.

Parsing fills in the developer's default level and compiler, so a request
carries exactly what will run whatever the defaults are later. Its canonical
command line, from to_argv, parses back to an equal request — the property
history, recipes, the Steam Runtime hand-off and the TUI's command bar all rely
on.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from typing import Any, Dict, List, Sequence, Tuple

from assisi.options import COMPILER, PROFILER, SANITIZE, STEAM_RUNTIME, Kind, Option
from assisi.project import BuildSpec, Project, UsageError

PASSTHROUGH_MARK = "--"


@dataclass(frozen=True)
class Request:
    command: str
    values: Tuple[Tuple[str, Any], ...]

    def get(self, name: str, fallback: Any = None) -> Any:
        return dict(self.values).get(name, fallback)

    def module(self):
        from assisi.commands import by_name
        return by_name()[self.command]

    def specs(self, project: Project) -> List[BuildSpec]:
        """Every build this request names, as level, compiler and flags."""
        values = dict(self.values)
        if "level" in values:
            levels = [values["level"]] if values["level"] is not None else []
        elif "levels" in values:
            levels = list(values["levels"])
        else:
            return []
        compiler = values.get(COMPILER) or project.default_compiler()
        return [BuildSpec(compiler, level, bool(values.get(PROFILER)), values.get(SANITIZE),
                          bool(values.get(STEAM_RUNTIME))) for level in levels]

    def trees(self, project: Project) -> List[str]:
        """The CMake presets those builds are."""
        return [project.tree_for(spec) for spec in self.specs(project)]

    def to_argv(self) -> List[str]:
        argv = [self.command]
        passthrough: Sequence[str] = ()
        for option in self.module().OPTIONS:
            value = self.get(option.name)
            if option.kind in (Kind.LEVEL, Kind.CHOICE, Kind.TEXT):
                if value is not None:
                    argv.append(value)
            elif option.kind is Kind.NUMBER:
                argv.append(str(value))
            elif option.kind is Kind.LEVELS:
                argv.extend(value)
            elif option.kind is Kind.VALUE:
                if value is not None:
                    argv += [option.flag, value]
            elif option.kind is Kind.FLAG:
                if value:
                    argv.append(option.flag)
            else:
                passthrough = value
        if passthrough:
            argv.append(PASSTHROUGH_MARK)
            argv.extend(passthrough)
        return argv


class _Parser(argparse.ArgumentParser):
    """argparse that reports a bad command line as a UsageError instead of
    exiting, so a recipe or a TUI form can be checked without ending the tool."""

    def error(self, message: str):
        raise UsageError(f"{self.prog}: {message}")


def make_parser(module) -> argparse.ArgumentParser:
    parser = _Parser(prog=f"assisi {module.NAME}", description=module.HELP, add_help=False)
    for option in module.OPTIONS:
        if option.hidden and option.kind is not Kind.FLAG:
            continue
        shown = argparse.SUPPRESS if option.hidden else option.help
        # Levels are checked against their choices in _resolve, not by argparse:
        # Python 3.9's argparse, the Steam Runtime container's, rejects an empty
        # nargs="*" list as an invalid choice. Optional to argparse even when
        # required, so a missing level is reported with the levels to choose from.
        if option.kind is Kind.LEVEL:
            parser.add_argument(option.name, nargs="?", help=shown, metavar="{" + ",".join(option.choices) + "}")
        elif option.kind is Kind.LEVELS:
            parser.add_argument(option.name, nargs="*", help=shown, metavar="{" + ",".join(option.choices) + "}")
        elif option.kind is Kind.TEXT:
            parser.add_argument(option.name, nargs=None if option.required else "?", help=shown,
                                metavar=option.name.upper())
        elif option.kind is Kind.CHOICE:
            parser.add_argument(option.name, nargs=None if option.required else "?", choices=option.choices,
                                help=shown)
        elif option.kind is Kind.NUMBER:
            parser.add_argument(option.name, nargs="?", type=int, help=shown, metavar=option.name.upper())
        elif option.kind is Kind.VALUE:
            parser.add_argument(option.flag, dest=option.name, choices=option.choices, default=None, help=shown)
        elif option.kind is Kind.FLAG:
            parser.add_argument(option.flag, dest=option.name, action="store_true", help=shown)
    return parser


def help_text(module) -> str:
    text = make_parser(module).format_help()
    passthrough = [option for option in module.OPTIONS if option.kind is Kind.PASSTHROUGH]
    if passthrough:
        text += f"\nAnything after {PASSTHROUGH_MARK} {passthrough[0].help}.\n"
    return text


def _check_levels(option: Option, levels: Sequence[str]) -> None:
    for level in levels:
        if level not in option.choices:
            raise UsageError(f"'{level}' is not a level. Levels: {', '.join(option.choices)}.")


def _resolve(option: Option, raw: Any, project: Project) -> Any:
    if option.kind in (Kind.LEVEL, Kind.LEVELS):
        _check_levels(option, [raw] if isinstance(raw, str) else (raw or []))
    if option.kind is Kind.LEVEL:
        if raw is None and option.required:
            raise UsageError(f"name the level: {', '.join(option.choices)}.")
        if raw is None and option.defaulted:
            return project.default_level()
        return raw
    if option.kind is Kind.LEVELS:
        if not raw and option.defaulted:
            return (project.default_level(),)
        return tuple(dict.fromkeys(raw or ()))
    if option.kind is Kind.VALUE:
        # The compiler is the one value with a default; a sanitizer is never assumed.
        if raw is None and option.name == COMPILER and option.defaulted:
            return project.default_compiler()
        return raw
    if option.kind is Kind.NUMBER:
        value = option.default if raw is None else raw
        if value < 1:
            raise UsageError(f"{option.name} must be 1 or more.")
        return value
    if option.kind is Kind.FLAG:
        return bool(raw)
    return raw


def parse(argv: Sequence[str], project: Project) -> Request:
    """The request @p argv describes, or a UsageError saying what is wrong."""
    from assisi.commands import by_name

    argv = list(argv)
    head, tail = argv, []
    if PASSTHROUGH_MARK in argv:
        split = argv.index(PASSTHROUGH_MARK)
        head, tail = argv[:split], argv[split + 1:]
    if not head:
        raise UsageError("no command given.")
    commands = by_name()
    module = commands.get(head[0])
    if module is None:
        raise UsageError(f"'{head[0]}' is not a command. Commands: {', '.join(sorted(commands))}.")
    passthrough = [option for option in module.OPTIONS if option.kind is Kind.PASSTHROUGH]
    if tail and not passthrough:
        raise UsageError(f"{module.NAME} takes nothing after {PASSTHROUGH_MARK}.")
    namespace = vars(make_parser(module).parse_args(head[1:]))
    values: Dict[str, Any] = {}
    for option in module.OPTIONS:
        if option.kind is Kind.PASSTHROUGH:
            values[option.name] = tuple(tail)
        else:
            values[option.name] = _resolve(option, namespace.get(option.name), project)
    result = Request(module.NAME, tuple((option.name, values[option.name]) for option in module.OPTIONS))
    # Refuses a level, compiler and flags no preset exists for, before anything runs.
    result.trees(project)
    validate = getattr(module, "validate", None)
    if validate is not None:
        validate(result, project)
    return result
