"""tui: open the TUI (what ./assisi does with no arguments in a terminal)."""

from __future__ import annotations

NAME = "tui"
HELP = "Open the TUI"
OPTIONS = ()
RECORDED = False


def execute(request, context) -> int:
    from assisi.tui.app import run
    return run(context)
