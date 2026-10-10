"""clean: delete build trees, or the shared dependency sources."""

from __future__ import annotations

import shutil

from assisi.options import Kind, Option, build_options
from assisi.project import UsageError, all_trees

NAME = "clean"
HELP = "Delete build folders so the next build starts from nothing"
OPTIONS = build_options(levels=True, level_defaulted=False) + (
    Option("all", Kind.FLAG, "delete every build folder"),
    Option("deps", Kind.FLAG, "delete the shared dependency sources, which is how a bumped pin takes effect"),
)
RECORDED = True


def validate(request, project) -> None:
    if request.get("levels") and request.get("all"):
        raise UsageError("name the levels to delete, or use --all, not both.")
    if not (request.get("levels") or request.get("all") or request.get("deps")):
        raise UsageError("name the levels to delete (e.g. clean debug), or use --all for every build or "
                         "--deps for the dependency sources.")


def remove(path, context) -> None:
    shown = path.relative_to(context.project.root)
    if path.exists():
        shutil.rmtree(path)
        print(f"removed {shown}")
    else:
        print(f"{shown} was not there")


def execute(request, context) -> int:
    if request.get("all"):
        for tree in all_trees():
            if context.project.build_dir(tree).exists():
                remove(context.project.build_dir(tree), context)
    else:
        for tree in request.trees(context.project):
            remove(context.project.build_dir(tree), context)
    if request.get("deps"):
        # Every configured tree points into this folder, so each one has to be
        # configured again before it builds; building does that by itself only
        # for a tree with no cache, so say it.
        remove(context.project.deps_dir(), context)
        print("Configure again before building: ./assisi configure <levels>")
    return 0
