"""history: what has been run, most recent first."""

from __future__ import annotations

import time

from assisi.process import describe
from assisi.store import rank_recent

NAME = "history"
HELP = "List what you have run, most recent first; ./assisi again N runs entry N"
OPTIONS = ()
RECORDED = False

# strftime pattern for when an entry last ran.
WHEN_FORMAT = "%Y-%m-%d %H:%M"


def execute(request, context) -> int:
    entries = rank_recent(context.store.history())
    if not entries:
        print("Nothing run yet.")
        return 0
    for number, entry in enumerate(entries, start=1):
        result = "ok    " if entry.last_ok else "failed"
        when = time.strftime(WHEN_FORMAT, time.localtime(entry.last_run))
        print(f"{number:>3}  {result}  {when}  ×{entry.count:<3}  ./assisi {describe(entry.argv)}")
    return 0
