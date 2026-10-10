"""The tool's commands, one module each, found by scanning this package.

A command is any module here not starting with an underscore. It declares:

  NAME      the word that runs it
  HELP      one line for `./assisi --help` and the TUI
  OPTIONS   a tuple of options.Option, from which its flags and form are made
  RECORDED  whether running it goes into history
  execute(request, context) -> int
  validate(request, project)   optional; raises UsageError for a bad combination

Adding a command is adding a file.
"""

from __future__ import annotations

import functools
import importlib
import pkgutil
from typing import Dict, List

INTERFACE = ("NAME", "HELP", "OPTIONS", "RECORDED", "execute")


@functools.lru_cache(maxsize=None)
def _load() -> tuple:
    modules = []
    for info in pkgutil.iter_modules(__path__):
        if info.name.startswith("_"):
            continue
        module = importlib.import_module(f"{__name__}.{info.name}")
        missing = [name for name in INTERFACE if not hasattr(module, name)]
        if missing:
            raise RuntimeError(f"command module {module.__name__} has no {', '.join(missing)}")
        modules.append(module)
    return tuple(sorted(modules, key=lambda module: module.NAME))


def catalog() -> List:
    return list(_load())


def by_name() -> Dict[str, object]:
    return {module.NAME: module for module in _load()}
