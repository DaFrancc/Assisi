"""run: start the editor or the game, building it first."""

from __future__ import annotations

from assisi.commands import _steps
from assisi.options import SANITIZE, STEAM_RUNTIME, Kind, Option, build_options
from assisi.project import UsageError

NAME = "run"
HELP = "Build and start the editor or the game"
OPTIONS = (Option("program", Kind.CHOICE, "what to start", required=True, choices=("editor", "game")),) \
    + build_options() + (
    Option("no_build", Kind.FLAG, "start what is already built without building first"),
    Option("program_arguments", Kind.PASSTHROUGH, "is handed to the program, e.g. -- -l levels/Test.alvl"),
)
RECORDED = True


def validate(request, project) -> None:
    if request.get("program") == "game" and request.get(SANITIZE):
        raise UsageError("a sanitizer build has no game; run the editor under it instead.")
    if request.get(STEAM_RUNTIME):
        raise UsageError("the Steam Runtime build is for other machines; test it with "
                         "./assisi test ship --steam-runtime.")


def execute(request, context) -> int:
    project = context.project
    tree = request.trees(project)[0]
    game = request.get("program") == "game"
    if not request.get("no_build"):
        _steps.ensure_configured(context, [tree])
        _steps.build(context, tree)
        if game:
            _steps.build(context, tree, (_steps.GAME_TARGET,))
    program = project.game_path(tree) if game else project.editor_path(tree)
    spec = " ".join(request.specs(project)[0].argv())
    if not program.exists():
        raise UsageError(f"{program} is not built. Build it with ./assisi build {spec}{' --game' if game else ''}.")
    if game and not project.pak_path(tree).exists():
        raise UsageError(f"there is no {project.pak_path(tree).name} beside the game. "
                         f"Make one with ./assisi package {spec}.")
    context.runner.run([program, *request.get("program_arguments")])
    return 0
