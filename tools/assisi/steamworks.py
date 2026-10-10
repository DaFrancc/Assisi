"""Installing the Steamworks SDK the developer downloaded from Valve.

Valve's agreement lets a developer copy the SDK locally to build their game and
ship the runtime libraries inside it, and nothing more, so the SDK never enters
git and the engine never downloads it. This copies the two parts a build needs
out of the developer's own download — the headers and the runtime libraries —
into a fixed, gitignored folder the build looks in, and leaves the rest of the
SDK (tools, the sample game, the API description) where it was.
"""

from __future__ import annotations

import re
import shutil
import sys
import zipfile
from pathlib import Path, PurePosixPath
from typing import Dict, Optional, Tuple

from assisi.project import Project, UsageError

# The folder under the repository root that cmake/AssisiSteamworks.cmake reads.
SDK_DIR = "steamworks"

# Where it is assembled before it replaces the old one, so a failed install
# never leaves half an SDK for the build to find.
PARTIAL_SUFFIX = ".partial"

HEADER_DIR = "public/steam"
API_HEADER = "steam_api.h"
LIBRARY_DIR = "redistributable_bin"

# The platforms the engine builds for. linux64 for every Linux build and the
# Steam Runtime one, win64 for Windows.
PLATFORMS = ("linux64", "win64")

# The oldest SDK the engine's Steam module compiles against: 1.65 replaced
# IsRunningOnSteamDeck with IsRunningOnSteamHardware, which the module calls.
MINIMUM_VERSION = (1, 65)

README = "Readme.txt"
VERSION_LINE = re.compile(r"^v(\d+)\.(\d+)", re.MULTILINE)
VERSION_FILE = "VERSION"


def sdk_dir(project: Project) -> Path:
    return project.root / SDK_DIR


class Download:
    """The developer's download, a zip or a folder, read without unpacking it."""

    def __init__(self, source: Path):
        self.source = source
        self.archive: Optional[zipfile.ZipFile] = None
        self.files: Dict[str, Path] = {}
        if not source.exists():
            raise UsageError(f"{source} does not exist. Give the zip or folder you downloaded from Valve.")
        if source.is_dir():
            self.names = [path.relative_to(source).as_posix() for path in source.rglob("*") if path.is_file()]
        elif zipfile.is_zipfile(source):
            self.archive = zipfile.ZipFile(source)
            self.names = [name for name in self.archive.namelist() if not name.endswith("/")]
        else:
            raise UsageError(f"{source} is neither a folder nor a zip file.")
        for name in self.names:
            path = PurePosixPath(name)
            if path.is_absolute() or ".." in path.parts:
                raise UsageError(f"{source} holds '{name}', which points outside it; refusing to read it.")

    def read(self, name: str) -> bytes:
        if self.archive is not None:
            return self.archive.read(name)
        return (self.source / name).read_bytes()

    def root(self) -> str:
        """The prefix the SDK's own folders sit under: '' for the sdk folder
        itself, 'sdk/' for the zip or the folder it unpacks to."""
        suffix = f"{HEADER_DIR}/{API_HEADER}"
        for name in self.names:
            if name == suffix or name.endswith("/" + suffix):
                return name[:-len(suffix)]
        raise UsageError(f"{self.source} has no {HEADER_DIR}/{API_HEADER}; is it the Steamworks SDK?")


def read_version(download: Download, root: str) -> Optional[Tuple[int, int]]:
    name = root + README
    if name not in download.names:
        return None
    found = VERSION_LINE.search(download.read(name).decode("utf-8", errors="replace"))
    return (int(found.group(1)), int(found.group(2))) if found else None


def wanted(name: str, root: str) -> Optional[str]:
    """Where @p name goes in the installed folder, or None to leave it out."""
    if not name.startswith(root):
        return None
    relative = PurePosixPath(name[len(root):])
    parent = relative.parent.as_posix()
    if parent == HEADER_DIR and relative.suffix == ".h":
        return relative.as_posix()
    if parent in (f"{LIBRARY_DIR}/{platform}" for platform in PLATFORMS):
        return relative.as_posix()
    return None


def install(project: Project, source: Path) -> Path:
    """Copy the needed parts of @p source into the repository's SDK folder,
    replacing what was there. Nothing changes unless the whole copy succeeds."""
    download = Download(source)
    root = download.root()
    version = read_version(download, root)
    if version is not None and version < MINIMUM_VERSION:
        needed = ".".join(map(str, MINIMUM_VERSION))
        raise UsageError(f"this is Steamworks SDK {version[0]}.{version[1]}; the engine needs {needed} or newer. "
                         "Download the current SDK from Valve.")
    if version is None:
        print(f"assisi: could not read the SDK's version from {README}; installing it anyway.", file=sys.stderr)
    target = sdk_dir(project)
    partial = target.with_name(SDK_DIR + PARTIAL_SUFFIX)
    if partial.exists():
        shutil.rmtree(partial)
    copied = 0
    for name in download.names:
        destination = wanted(name, root)
        if destination is None:
            continue
        path = partial / destination
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(download.read(name))
        copied += 1
    shown = f"{version[0]}.{version[1]}" if version else "unknown"
    (partial / VERSION_FILE).write_text(shown + "\n")
    if target.exists():
        shutil.rmtree(target)
    partial.rename(target)
    print(f"Installed Steamworks SDK {shown}: {copied} files in {target.relative_to(project.root)}/ "
          "(headers and runtime libraries only).")
    return target
