"""steam-sdk: install the Steamworks SDK you downloaded from Valve."""

from __future__ import annotations

from assisi import steamworks
from assisi.options import Kind, Option

NAME = "steam-sdk"
HELP = "Install the Steamworks SDK zip or folder you downloaded from Valve (see book/src/steam.md)"
OPTIONS = (Option("sdk", Kind.TEXT, "the SDK zip or folder you downloaded", required=True),)
RECORDED = False


def execute(request, context) -> int:
    steamworks.install(context.project, context.project.path_from(request.get("sdk")))
    print("Next: build as usual, e.g. ./assisi build. The build finds the SDK by itself.")
    return 0
