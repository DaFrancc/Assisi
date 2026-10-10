"""recipes: list the saved commands."""

from __future__ import annotations

from assisi.process import describe

NAME = "recipes"
HELP = "List your recipes (saved commands); ./assisi forget <name> deletes one"
OPTIONS = ()
RECORDED = False


def execute(request, context) -> int:
    recipes = context.store.recipes()
    if not recipes:
        print("No recipes yet. Save one with ./assisi save <name> -- <command>.")
        return 0
    width = max(len(name) for name in recipes)
    for name, argv in recipes.items():
        print(f"{name:<{width}}  ./assisi {describe(argv)}")
    return 0
