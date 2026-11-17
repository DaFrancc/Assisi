"""test: build, then run the build's tests."""

from __future__ import annotations

import os
from typing import List

from assisi.commands import _steps
from assisi.options import TARGET, Kind, Option, build_options, target_option, targets_of
from assisi.project import STEAMRT_BUILD, UsageError

NAME = "test"
HELP = "Build, then run the build's tests with ctest; --gpu runs the GPU tests instead"
GPU = "gpu"
ACCEPT = "accept"
OPTIONS = build_options() + (
    target_option("build only this test suite and run only its tests, e.g. Assisi-Runtime-Tests"),
    Option(GPU, Kind.FLAG, "run only the GPU tests: every level drawn under the Vulkan validation layer and "
                           "compared with the picture last accepted on this GPU. Each opens a window"),
    Option(ACCEPT, Kind.FLAG, "with --gpu, accept each picture drawn as the one to compare with from now on"),
    Option("ctest_arguments", Kind.PASSTHROUGH, "is handed to ctest, e.g. -- -R Audio"),
)
RECORDED = True

# The GPU tests' label. They open windows and compare with pictures kept on this
# machine alone, so a plain run leaves them out.
GPU_LABEL = "gpu"

# What makes the GPU tests accept their pictures instead of comparing; the
# editor reads it, so the tests' own command lines stay the same.
ACCEPT_ENVIRONMENT = "ASSISI_ACCEPT_GOLDEN=1"

# Inside the container only the game's tests run, and the build tool's own: the
# build exists to prove the game starts on an old glibc, and the tool runs on the
# container's old Python; the rest of the suite proves nothing new there.
STEAMRT_LABEL = ("-L", "game|container")

# The ways ctest is told how many tests to run at once; any of them passed
# through replaces the tool's own count.
JOBS_FLAGS = ("-j", "--parallel")

# The characters CMake allows in a target name that mean something in the
# regular expression ctest matches labels with.
LABEL_SPECIAL = ".+"


def parallel_jobs() -> int:
    """How many tests run at once: one per CPU, as the build compiles."""
    return os.cpu_count() or 1


def names_jobs(arguments) -> bool:
    return any(argument.startswith(flag) for argument in arguments for flag in JOBS_FLAGS)


def suite_filter(target: str) -> List[str]:
    """ctest's arguments for running only @p target's tests: each suite's tests
    are labelled with its executable. Only the named suite was built, so any
    other suite's tests would run a stale executable, and a target with no tests
    is an error rather than a pass with nothing run."""
    escaped = "".join("\\" + character if character in LABEL_SPECIAL else character for character in target)
    return ["-L", f"^{escaped}$", "--no-tests=error"]


def validate(request, project) -> None:
    gpu = request.get(GPU)
    if request.get(ACCEPT) and not gpu:
        raise UsageError("--accept accepts the GPU tests' pictures; give --gpu with it.")
    if gpu and request.get(TARGET):
        raise UsageError("--gpu runs the editor's GPU tests and --target one test program's; give one or the other.")
    if gpu and STEAMRT_BUILD in request.trees(project):
        raise UsageError("--gpu opens windows, and the Steam Runtime container has no display to open them on.")


def label_filter(request) -> List[str]:
    """The GPU tests alone with --gpu, and everything else without it."""
    return ["-L", GPU_LABEL] if request.get(GPU) else ["-LE", GPU_LABEL]


def execute(request, context) -> int:
    tree = request.trees(context.project)[0]
    target = request.get(TARGET)
    forwarded = _steps.forward_to_container(context, request)
    if forwarded is not None:
        # The container cannot start another container, so booting the staged
        # game on a bare old system happens from here, once its tests passed.
        # One target alone stages no game, so there is nothing new to boot.
        if not target:
            _steps.steamrt_helper(context, "boot-check")
        return 0
    _steps.ensure_configured(context, [tree])
    _steps.build(context, tree, targets_of(request))
    passthrough = list(request.get("ctest_arguments"))
    argv = ["ctest", "--preset", tree]
    if not names_jobs(passthrough):
        argv.append(f"-j{parallel_jobs()}")
    argv += label_filter(request)
    if target:
        argv += suite_filter(target)
    elif tree == STEAMRT_BUILD:
        argv += list(STEAMRT_LABEL)
    if request.get(ACCEPT):
        argv = ["cmake", "-E", "env", ACCEPT_ENVIRONMENT, *argv]
    context.runner.run(argv + passthrough)
    return 0
