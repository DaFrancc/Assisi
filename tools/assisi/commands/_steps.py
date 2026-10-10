"""The build steps commands are made of: configure, build, cook, pack, and the
hand-off of a Steam Runtime build to its container."""

from __future__ import annotations

import sys
from typing import List, Optional, Sequence

from assisi.core import Context
from assisi.project import CODECS, STEAMRT_BUILD, TIERS, UsageError, compiler_of, config_of
from assisi.request import Request

GAME_TARGET = "Assisi-Game"
COOK_TARGETS = ("Assisi-Cook-Tool", "Assisi-VkShaders")
PACK_TARGET = "Assisi-Pack-Tool"


def configure(context: Context, trees: Sequence[str]) -> None:
    """Configure @p trees through the shared dependency cache, one call per
    compiler, as each compiler's presets share a toolchain."""
    groups: List[List[str]] = []
    for tree in trees:
        group = next((group for group in groups if compiler_of(group[0]) == compiler_of(tree)), None)
        if group is None:
            groups.append([tree])
        else:
            group.append(tree)
    for group in groups:
        context.runner.run(["cmake", f"-DPRESETS={';'.join(group)}", "-P", context.project.configure_script()])


def ensure_configured(context: Context, trees: Sequence[str]) -> None:
    """Configure whichever of @p trees has never been, so building never starts
    with a preset CMake has no tree for."""
    missing = [tree for tree in trees if not context.project.is_configured(tree)]
    if missing:
        configure(context, missing)


def build(context: Context, tree: str, targets: Sequence[str] = ()) -> None:
    argv = ["cmake", "--build", "--preset", tree]
    if targets:
        argv += ["--target", *targets]
    context.runner.run(argv)


def cook(context: Context, tree: str) -> None:
    project = context.project
    build(context, tree, COOK_TARGETS)
    context.runner.run([project.cook_tool(tree), "--source", project.assets_dir(), "--out", project.cooked_dir(tree),
                        "--texture-tier", TIERS[config_of(tree)]])


def pack(context: Context, tree: str) -> None:
    project = context.project
    build(context, tree, (PACK_TARGET,))
    context.runner.run([project.pack_tool(tree), "--cooked", project.cooked_dir(tree), "--out",
                        project.pak_path(tree), "--compress", CODECS[config_of(tree)]])


def forward_to_container(context: Context, request: Request) -> Optional[int]:
    """Run @p request inside the Steam Runtime container when it names that build
    and this is the host; None when it should run here instead.

    The repository is mounted at the same path inside, so the request's own
    canonical command line is what runs there.
    """
    builds = request.trees(context.project)
    if STEAMRT_BUILD not in builds or context.project.in_steamrt_container():
        return None
    if len(builds) > 1:
        raise UsageError(f"{STEAMRT_BUILD} runs in its own container; name it on its own.")
    context.runner.run([sys.executable, context.project.steamrt_helper(), "run", "--", "python3",
                        context.project.launcher(), *request.to_argv()])
    return 0


def steamrt_helper(context: Context, *arguments: str) -> None:
    context.runner.run([sys.executable, context.project.steamrt_helper(), *arguments])
