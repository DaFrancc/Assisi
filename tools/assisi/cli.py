"""The command line: ./assisi <command> [options], or a recipe's name.

With no arguments in an interactive terminal it opens the TUI, and the first
time it runs at one in a checkout it runs setup first. Anywhere else — a
script, a pipe, CI, the Steam Runtime container — it is a plain command line
that never prompts, and with no arguments prints what it can do.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import List, Optional

from assisi import core, request, setup
from assisi.commands import by_name, catalog
from assisi.process import Runner, describe
from assisi.project import CONFIGS, Project, UsageError
from assisi.store import Store

HELP_WORDS = ("-h", "--help", "help")

# The command that runs setup on purpose, which the first run does not run first.
SETUP_COMMAND = "setup"


def overview(context: core.Context) -> str:
    width = max(len(module.NAME) for module in catalog())
    lines = ["Usage: ./assisi <command> [options]   (./assisi <command> --help for its options)", "",
             "Commands:"]
    lines += [f"  {module.NAME:<{width}}  {module.HELP}" for module in catalog()]
    recipes = context.store.recipes()
    if recipes:
        lines += ["", "Your recipes:"]
        recipe_width = max(len(name) for name in recipes)
        lines += [f"  {name:<{recipe_width}}  ./assisi {describe(argv)}" for name, argv in recipes.items()]
    project = context.project
    lines += ["", f"A build is a level ({', '.join(CONFIGS)}) and a compiler (--compiler "
              f"{'|'.join(project.compilers())}), plus --profiler, --sanitize address|thread or --steam-runtime.",
              f"Your default: {project.default_level()} with {project.default_compiler()} "
              "(./assisi default changes it).",
              "", "With no command in a terminal, ./assisi opens the TUI."]
    return "\n".join(lines)


def asks_for_help(argv: List[str]) -> bool:
    """Whether @p argv asks for help, overall or with a command's own --help."""
    if argv[:1] and argv[0] in HELP_WORDS:
        return True
    if not argv or argv[0] not in by_name():
        return False
    own_words = argv[1:argv.index(request.PASSTHROUGH_MARK)] if request.PASSTHROUGH_MARK in argv else argv[1:]
    return any(word in HELP_WORDS[:2] for word in own_words)


def setup_due(project: Project, interactive: bool, argv: List[str]) -> bool:
    """Whether to run first-time setup before @p argv: at a terminal, once per
    checkout, never for help or setup itself, and never inside the Steam
    Runtime container, which the host's own run has already set up."""
    if not interactive or project.in_steamrt_container() or asks_for_help(argv):
        return False
    return argv[:1] != [SETUP_COMMAND] and not setup.offered(project)


def main(argv: Optional[List[str]] = None, root: Optional[Path] = None, host: Optional[str] = None,
         interactive: Optional[bool] = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    root = Path(root) if root is not None else Path(__file__).resolve().parents[2]
    project = Project(root, host=host)
    if interactive is None:
        interactive = sys.stdin.isatty() and sys.stdout.isatty()
    context = core.Context(project, Runner(root), Store(root), interactive)

    if setup_due(project, interactive, argv):
        setup.mark_offered(project)
        try:
            setup.run(context)
        except KeyboardInterrupt:
            print()
            return core.INTERRUPTED_EXIT

    if not argv:
        if interactive:
            from assisi.tui import term
            if term.is_supported():
                from assisi.tui.app import run
                return run(context)
        print(overview(context))
        return 0

    commands = by_name()
    if argv[0] in HELP_WORDS:
        topic = commands.get(argv[1]) if len(argv) > 1 else None
        print(request.help_text(topic) if topic else overview(context))
        return 0
    if asks_for_help(argv):
        print(request.help_text(commands[argv[0]]))
        return 0
    if argv[0] not in commands:
        recipe = context.store.recipes().get(argv[0])
        if recipe is not None:
            if len(argv) > 1:
                return core.report_usage(UsageError(f"the recipe {argv[0]} takes no arguments."))
            print(f"{argv[0]}: ./assisi {describe(recipe)}")
            argv = recipe
    try:
        parsed = request.parse(argv, project)
    except UsageError as error:
        return core.report_usage(error)
    return core.execute(parsed, context)
