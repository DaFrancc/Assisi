"""save: name a command so it runs as ./assisi <name>."""

from __future__ import annotations

from assisi.options import Kind, Option
from assisi.project import UsageError
from assisi.store import rank_recent

NAME = "save"
HELP = "Save a command as a recipe: ./assisi save ship-it -- package gcc-ship, then ./assisi ship-it"
OPTIONS = (
    Option("name", Kind.TEXT, "what to call the recipe", required=True),
    Option("command", Kind.PASSTHROUGH, "is the command to save (default: the last one you ran)"),
)
RECORDED = False


def name_problem(name: str, project) -> str:
    """Why @p name cannot be a recipe, or an empty string when it can."""
    from assisi.commands import by_name

    if not name or name.startswith("-") or any(character.isspace() for character in name):
        return "a recipe name is one word that does not start with -."
    if name in by_name():
        return f"'{name}' is a command."
    if project.is_build_word(name):
        return f"'{name}' is a level, compiler or build name."
    return ""


def recipe_argv(request, context) -> list:
    """The canonical command line the request asks to save."""
    from assisi.commands import by_name
    from assisi.request import parse

    command = list(request.get("command"))
    if not command:
        entries = rank_recent(context.store.history())
        if not entries:
            raise UsageError("nothing has been run yet; give the command after --.")
        command = entries[0].argv
    parsed = parse(command, context.project)
    if not by_name()[parsed.command].RECORDED:
        raise UsageError(f"{parsed.command} cannot be saved as a recipe.")
    return parsed.to_argv()


def execute(request, context) -> int:
    name = request.get("name")
    problem = name_problem(name, context.project)
    if problem:
        raise UsageError(problem)
    argv = recipe_argv(request, context)
    context.store.save_recipe(name, argv)
    print(f"saved: ./assisi {name}  =  ./assisi {' '.join(argv)}")
    return 0
