"""pack: bundle the last cook into assets.pak beside the game."""

from __future__ import annotations

from assisi.commands import _steps
from assisi.options import build_options, previous_options

NAME = "pack"
HELP = "Pack the build's last cook into assets.pak beside the game"
OPTIONS = build_options(sanitizers=False) + previous_options()
RECORDED = True


def validate(request, project) -> None:
    _steps.check_previous(request)


def execute(request, context) -> int:
    forwarded = _steps.forward_to_container(context, request)
    if forwarded is not None:
        return forwarded
    tree = request.trees(context.project)[0]
    _steps.ensure_configured(context, [tree])
    _steps.pack(context, tree, _steps.previous_pak(context, request))
    return 0
