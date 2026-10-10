"""forget: delete a recipe."""

from __future__ import annotations

from assisi.options import Kind, Option
from assisi.project import UsageError

NAME = "forget"
HELP = "Delete a recipe"
OPTIONS = (Option("name", Kind.TEXT, "the recipe to delete", required=True),)
RECORDED = False


def execute(request, context) -> int:
    name = request.get("name")
    if not context.store.forget_recipe(name):
        raise UsageError(f"there is no recipe called '{name}'.")
    print(f"forgot {name}")
    return 0
