"""default: show or set the level and compiler used when none is named."""

from __future__ import annotations

from assisi.options import Kind, Option
from assisi.project import COMPILER_KEY, COMPILERS, CONFIGS, LEVEL_KEY, BuildSpec

NAME = "default"
HELP = "Show your default level and compiler, or set them (they are yours alone and never committed)"
OPTIONS = (
    Option("level", Kind.LEVEL, "the level to make the default", choices=CONFIGS, defaulted=False),
    Option("compiler", Kind.VALUE, "the compiler to make the default", choices=COMPILERS, defaulted=False),
)
RECORDED = False


def execute(request, context) -> int:
    project = context.project
    level, compiler = request.get("level"), request.get("compiler")
    if level is not None or compiler is not None:
        if compiler is not None:
            # Through tree_for, so a compiler this host cannot build with is refused.
            project.tree_for(BuildSpec(compiler, level or project.default_level()))
        settings = project.read_settings()
        if level is not None:
            settings[LEVEL_KEY] = level
        if compiler is not None:
            settings[COMPILER_KEY] = compiler
        project.write_settings(settings)
    print(f"default: {project.default_level()} with {project.default_compiler()}")
    return 0
