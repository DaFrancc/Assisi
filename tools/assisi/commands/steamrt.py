"""steamrt: download or delete the Steam Runtime SDK the release build uses."""

from __future__ import annotations

import shutil

from assisi.commands import _steps
from assisi.options import Kind, Option

NAME = "steamrt"
HELP = "Download (fetch) or delete (remove) the Steam Runtime SDK images and their cache"
OPTIONS = (Option("action", Kind.CHOICE, "fetch or remove", required=True, choices=("fetch", "remove")),)
RECORDED = True


def execute(request, context) -> int:
    if request.get("action") == "fetch":
        _steps.steamrt_helper(context, "prepare")
        return 0
    _steps.steamrt_helper(context, "remove")
    # The container's home holds its compiler cache, so it goes with the images.
    home = context.project.steamrt_home()
    if home.exists():
        shutil.rmtree(home)
    return 0
