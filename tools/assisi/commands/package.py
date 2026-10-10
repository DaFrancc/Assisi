"""package: the game, cooked and packed — the folder a player receives."""

from __future__ import annotations

from assisi.commands import _steps
from assisi.options import build_options, previous_options

NAME = "package"
HELP = "Build the game, cook the assets and pack them: the two files a player needs"
OPTIONS = build_options(sanitizers=False, required=True) + previous_options()
RECORDED = True


def validate(request, project) -> None:
    _steps.check_previous(request)


def execute(request, context) -> int:
    forwarded = _steps.forward_to_container(context, request)
    if forwarded is not None:
        return forwarded
    tree = request.trees(context.project)[0]
    _steps.ensure_configured(context, [tree])
    _steps.build(context, tree)
    _steps.build(context, tree, (_steps.GAME_TARGET,))
    _steps.cook(context, tree)
    _steps.pack(context, tree, _steps.previous_pak(context, request))
    return 0
