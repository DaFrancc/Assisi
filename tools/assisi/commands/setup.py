"""setup: run first-time setup again."""

from __future__ import annotations

from assisi import core
from assisi.project import UsageError

NAME = "setup"
HELP = "Check this machine, pick your defaults and make `assisi` a command you can type anywhere"
OPTIONS = ()
RECORDED = False


def execute(request, context) -> int:
    if not context.interactive:
        return core.report_usage(UsageError("setup asks questions, so it only runs in a terminal."))
    from assisi import setup
    return setup.run(context)
