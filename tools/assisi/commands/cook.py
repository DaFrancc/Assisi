"""cook: convert assets/ into the form the game loads, for one build."""

from __future__ import annotations

from assisi.commands import _steps
from assisi.options import build_options

NAME = "cook"
HELP = "Cook assets/ into the build's cooked/ folder (only changed assets are redone)"
OPTIONS = build_options(sanitizers=False)
RECORDED = True


def execute(request, context) -> int:
    forwarded = _steps.forward_to_container(context, request)
    if forwarded is not None:
        return forwarded
    tree = request.trees(context.project)[0]
    _steps.ensure_configured(context, [tree])
    _steps.cook(context, tree)
    return 0
