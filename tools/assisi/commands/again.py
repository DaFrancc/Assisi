"""again: run something from history again."""

from __future__ import annotations

from assisi.options import Kind, Option
from assisi.project import UsageError
from assisi.store import rank_recent

NAME = "again"
HELP = "Run the last command again, or entry N from ./assisi history"
OPTIONS = (Option("number", Kind.NUMBER, "which history entry, 1 being the most recent"),)
RECORDED = False


def execute(request, context) -> int:
    from assisi import core, request as requests

    entries = rank_recent(context.store.history())
    number = request.get("number")
    if number > len(entries):
        raise UsageError(f"history has {len(entries)} entries." if entries else "nothing has been run yet.")
    return core.execute(requests.parse(entries[number - 1].argv, context.project), context)
