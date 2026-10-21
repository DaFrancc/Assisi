"""test: build, then run the build's tests."""

from __future__ import annotations

import os

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

# The ways ctest is told how many tests to run at once; any of them passed
# through replaces the tool's own count.
JOBS_FLAGS = ("-j", "--parallel")


def parallel_jobs() -> int:
    """How many tests run at once: one per CPU, as the build compiles."""
    return os.cpu_count() or 1


def names_jobs(arguments) -> bool:
    return any(argument.startswith(flag) for argument in arguments for flag in JOBS_FLAGS)


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
    passthrough = list(request.get("ctest_arguments"))
    argv = ["ctest", "--preset", tree]
    if not names_jobs(passthrough):
        argv.append(f"-j{parallel_jobs()}")
    if tree == STEAMRT_BUILD:
        argv += list(STEAMRT_LABEL)
    context.runner.run(argv + passthrough)
    return 0
