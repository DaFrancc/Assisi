"""configure: set up build trees, sharing one copy of the dependency sources."""

from __future__ import annotations

from assisi.commands import _steps
from assisi.options import build_options

NAME = "configure"
HELP = "Configure builds again, even ones already configured (building configures a new one by itself)"
OPTIONS = build_options(levels=True)
RECORDED = True


def execute(request, context) -> int:
    forwarded = _steps.forward_to_container(context, request)
    if forwarded is not None:
        return forwarded
    _steps.configure(context, request.trees(context.project))
    return 0
