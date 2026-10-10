"""tui: open the TUI (what ./assisi does with no arguments in a terminal)."""

from __future__ import annotations

from assisi.options import Kind, Option

NAME = "tui"
HELP = "Open the TUI; --reinstall rebuilds its environment first"
OPTIONS = (
    Option("reinstall", Kind.FLAG, "rebuild the TUI's Python environment before opening it"),
    # Set by the tool when it restarts itself inside that environment.
    Option("in_env", Kind.FLAG, "run the TUI in this interpreter", hidden=True),
)
RECORDED = False

# What the command exits with when the TUI cannot be started.
NOT_STARTED = 1


def execute(request, context) -> int:
    if request.get("in_env"):
        # The only import of the TUI, so nothing else ever loads Textual.
        from assisi.tui.app import run
        return run(context)
    from assisi import toolenv
    code = toolenv.start(context.project, request.get("reinstall"))
    return NOT_STARTED if code is None else code
