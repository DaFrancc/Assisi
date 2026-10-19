"""What the file browser lists, apart from drawing it.

A folder's entries come back folders first, each marked with whether it can be
opened: a folder needs reading and entering, a file reading. What cannot be
opened is still listed, so the developer sees why it is not there to pick.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import List

from assisi.project import Project

# The entry that goes up a folder.
PARENT = ".."


@dataclass(frozen=True)
class Entry:
    name: str
    path: Path
    is_dir: bool
    readable: bool


def can_open(path: Path, is_dir: bool) -> bool:
    return os.access(path, os.R_OK | os.X_OK) if is_dir else os.access(path, os.R_OK)


def entry_for(path: Path, name: str) -> Entry:
    try:
        is_dir = path.is_dir()
    except OSError:
        is_dir = False
    return Entry(name, path, is_dir, can_open(path, is_dir))


def listing(directory: Path) -> List[Entry]:
    """The entries of @p directory: the way up, then its folders, then its
    files, each group by name. A folder that cannot be read lists only the way up."""
    entries = [entry_for(directory.parent, PARENT)] if directory.parent != directory else []
    try:
        children = [entry_for(child, child.name) for child in directory.iterdir()]
    except OSError:
        return entries
    children.sort(key=lambda entry: (not entry.is_dir, entry.name.lower()))
    return entries + children


def start_directory(project: Project, given: str) -> Path:
    """Where browsing starts: beside the file already chosen, else among the
    releases, which is where an earlier pak is, else the repository root."""
    if given:
        folder = project.path_from(given).parent
        if folder.is_dir():
            return folder
    releases = project.releases_dir()
    return releases if releases.is_dir() else project.root


def as_given(project: Project, path: Path) -> str:
    """@p path as the command line takes it: from the repository root when it
    is inside it, which keeps a recipe working in another clone, else whole.
    Both are resolved first, since the browser shows folders resolved and the
    root may be reached through a link."""
    resolved = path.resolve()
    try:
        return str(resolved.relative_to(project.root.resolve()))
    except ValueError:
        return str(resolved)
