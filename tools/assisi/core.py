"""Running a request, the same way from the command line and from the TUI."""

from __future__ import annotations

import sys
import time
from dataclasses import dataclass

from assisi.process import CommandFailed, Runner
from assisi.project import Project, UsageError
from assisi.request import Request
from assisi.store import Store

# The exit code for a request refused before anything ran, as argparse uses.
USAGE_EXIT = 2


@dataclass
class Context:
    project: Project
    runner: Runner
    store: Store


def report_usage(error: UsageError) -> int:
    print(f"assisi: {error}", file=sys.stderr)
    return USAGE_EXIT


def execute(request: Request, context: Context) -> int:
    """Run @p request and return its exit code. Requests that build, test or run
    something are recorded in history with how they ended; one refused before it
    started is not, since nothing it names happened."""
    module = request.module()
    started = time.monotonic()
    try:
        code = module.execute(request, context)
    except CommandFailed as failed:
        code = failed.code
    except UsageError as error:
        return report_usage(error)
    if module.RECORDED:
        context.store.record(request.to_argv(), code == 0, time.monotonic() - started)
    return code
