"""Releases: what players were given, kept so the next package can match it.

Each release is a folder under releases/ holding the game, its assets.pak and a
release.json saying which build it came from. Packing lays a new pak out
against the newest release of the same level, which keeps every unchanged asset
where players already have it, so a store's patch is only what changed.
"""

from __future__ import annotations

import json
import shutil
import time
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

from assisi.project import PAK_NAME, BuildSpec, Project, UsageError

RECORD = "release.json"

# Suffix of the folder a release is assembled in before it takes its name, so a
# copy that fails part-way never looks like a release.
PARTIAL_SUFFIX = ".partial"


@dataclass(frozen=True)
class Release:
    version: str
    level: str
    created: float
    folder: Path

    @property
    def pak(self) -> Path:
        return self.folder / PAK_NAME


def all_releases(project: Project) -> List[Release]:
    found = []
    folder = project.releases_dir()
    if not folder.is_dir():
        return found
    for entry in folder.iterdir():
        try:
            record = json.loads((entry / RECORD).read_text())
            found.append(Release(str(record["version"]), str(record["level"]), float(record["created"]), entry))
        except (OSError, ValueError, KeyError, TypeError):
            continue
    return found


def latest(project: Project, level: str) -> Optional[Release]:
    """The newest release of @p level that still has its pak."""
    candidates = [release for release in all_releases(project) if release.level == level and release.pak.is_file()]
    return max(candidates, key=lambda release: release.created, default=None)


def check_version(project: Project, version: str) -> None:
    if not version or version in (".", "..") or any(character in version for character in "/\\") \
            or version.endswith(PARTIAL_SUFFIX):
        raise UsageError(f"'{version}' cannot be a release version; use something like 1.2 or 2026-10-early-access.")
    if (project.releases_dir() / version).exists():
        raise UsageError(f"release {version} already exists in {project.releases_dir().name}/. "
                         "Releases are never overwritten; pick a new version.")


def keep(project: Project, version: str, spec: BuildSpec, tree: str, argv: List[str]) -> Path:
    """Copy the packaged game of @p tree into releases/<version>/."""
    target = project.releases_dir() / version
    partial = target.with_name(version + PARTIAL_SUFFIX)
    if partial.exists():
        shutil.rmtree(partial)
    partial.mkdir(parents=True)
    shutil.copy2(project.game_path(tree), partial / project.game_path(tree).name)
    shutil.copy2(project.pak_path(tree), partial / PAK_NAME)
    record = {"version": version, "level": spec.level, "compiler": spec.compiler,
              "steam_runtime": spec.steam_runtime, "created": time.time(), "command": argv}
    (partial / RECORD).write_text(json.dumps(record, indent=2) + "\n")
    partial.rename(target)
    return target
