"""test: build, then run the build's tests."""

from __future__ import annotations

from assisi.commands import _steps
from assisi.options import Kind, Option, build_options
from assisi.project import STEAMRT_BUILD

NAME = "test"
HELP = "Build, then run the build's tests with ctest"
OPTIONS = build_options() + (
    Option("ctest_arguments", Kind.PASSTHROUGH, "is handed to ctest, e.g. -- -R Audio"),
)
RECORDED = True

# Inside the container only the game's tests run, and the build tool's own: the
# build exists to prove the game starts on an old glibc, and the tool runs on the
# container's old Python; the rest of the suite proves nothing new there.
STEAMRT_LABEL = ("-L", "game|container")


def execute(request, context) -> int:
    tree = request.trees(context.project)[0]
    forwarded = _steps.forward_to_container(context, request)
    if forwarded is not None:
        # The container cannot start another container, so booting the staged
        # game on a bare old system happens from here, once its tests passed.
        _steps.steamrt_helper(context, "boot-check")
        return 0
    _steps.ensure_configured(context, [tree])
    _steps.build(context, tree)
    argv = ["ctest", "--preset", tree]
    if tree == STEAMRT_BUILD:
        argv += list(STEAMRT_LABEL)
    context.runner.run(argv + list(request.get("ctest_arguments")))
    return 0
