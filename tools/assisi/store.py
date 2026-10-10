"""The developer's own history and recipes, in .assisi/ at the repository root.

At the root rather than under out/ so that cleaning every build keeps them.
Both files are the tool's to write; one that will not parse is reported and
started over rather than stopping every command until somebody edits JSON.
"""

from __future__ import annotations

import json
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Dict, List, Optional

from assisi.project import SETTINGS_DIR

HISTORY_FILE = "history.json"
RECIPES_FILE = "recipes.json"

# How many different requests history remembers. Enough to hold every build,
# step and test somebody runs in a working week; the least recently run goes.
HISTORY_LIMIT = 50


@dataclass
class Entry:
    """One request as history holds it: its canonical command line and record."""

    argv: List[str]
    count: int
    last_run: float
    last_ok: bool
    duration: float


def rank_common(entries: List[Entry]) -> List[Entry]:
    """Most often run first; among equals, the most recent."""
    return sorted(entries, key=lambda entry: (-entry.count, -entry.last_run))


def rank_recent(entries: List[Entry]) -> List[Entry]:
    return sorted(entries, key=lambda entry: -entry.last_run)


class Store:
    def __init__(self, root: Path):
        self.folder = Path(root) / SETTINGS_DIR

    def _read(self, name: str, empty):
        path = self.folder / name
        if not path.exists():
            return empty
        try:
            value = json.loads(path.read_text())
        except (OSError, ValueError):
            print(f"assisi: {path} could not be read; starting it over.", file=sys.stderr)
            return empty
        if not isinstance(value, type(empty)):
            print(f"assisi: {path} is not what the tool writes; starting it over.", file=sys.stderr)
            return empty
        return value

    def _write(self, name: str, value) -> None:
        self.folder.mkdir(parents=True, exist_ok=True)
        temporary = self.folder / (name + ".tmp")
        temporary.write_text(json.dumps(value, indent=2) + "\n")
        temporary.replace(self.folder / name)

    # --- History

    def history(self) -> List[Entry]:
        entries = []
        for item in self._read(HISTORY_FILE, []):
            try:
                entries.append(Entry(argv=list(item["argv"]), count=int(item["count"]),
                                     last_run=float(item["last_run"]), last_ok=bool(item["last_ok"]),
                                     duration=float(item.get("duration", 0.0))))
            except (KeyError, TypeError, ValueError):
                continue
        return entries

    def record(self, argv: List[str], ok: bool, duration: float, now: Optional[float] = None) -> None:
        moment = time.time() if now is None else now
        entries = self.history()
        match = next((entry for entry in entries if entry.argv == argv), None)
        if match is None:
            entries.append(Entry(argv=list(argv), count=1, last_run=moment, last_ok=ok, duration=duration))
        else:
            match.count += 1
            match.last_run = moment
            match.last_ok = ok
            match.duration = duration
        kept = rank_recent(entries)[:HISTORY_LIMIT]
        self._write(HISTORY_FILE, [asdict(entry) for entry in kept])

    # --- Recipes

    def recipes(self) -> Dict[str, List[str]]:
        raw = self._read(RECIPES_FILE, {})
        return {name: list(argv) for name, argv in raw.items() if isinstance(argv, list)}

    def save_recipe(self, name: str, argv: List[str]) -> None:
        recipes = self.recipes()
        recipes[name] = list(argv)
        self._write(RECIPES_FILE, dict(sorted(recipes.items())))

    def forget_recipe(self, name: str) -> bool:
        recipes = self.recipes()
        if name not in recipes:
            return False
        del recipes[name]
        self._write(RECIPES_FILE, recipes)
        return True
