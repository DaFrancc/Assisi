"""build: the editor, the tests and the shaders, and optionally the game."""

from __future__ import annotations

from assisi.commands import _steps
from assisi.options import Kind, Option, build_options, target_option, targets_of

NAME = "build"
HELP = ("Build the editor, tests and shaders; --game also builds the game without the editor, "
        "and --target builds one target alone")
OPTIONS = build_options(levels=True) + (
    Option("game", Kind.FLAG, "also build Assisi-Game, the game a player runs"),
    target_option("build only this CMake target and what it needs, e.g. Assisi-Runtime-Tests"),
)
RECORDED = True


def execute(request, context) -> int:
    forwarded = _steps.forward_to_container(context, request)
    if forwarded is not None:
        return forwarded
    trees = request.trees(context.project)
    _steps.ensure_configured(context, trees)
    for tree in trees:
        _steps.build(context, tree, targets_of(request))
        if request.get("game"):
            _steps.build(context, tree, (_steps.GAME_TARGET,))
    return 0
