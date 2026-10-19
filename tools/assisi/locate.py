"""Which checkout a typed `assisi` command belongs to.

The command setup installs runs from any folder. Inside an Assisi checkout it
hands off to that checkout's own tool, which matches that checkout's engine;
anywhere else it runs the checkout it was installed from. A tool started by
its path, ./assisi or /path/to/a/assisi, always runs its own checkout, so a
script that names one checkout is never moved to another.

The repository's assisi script imports this before the rest of the tool, so it
uses nothing but the standard library and imports nothing of the tool's.
"""

from __future__ import annotations

from pathlib import Path
from typing import Mapping, Optional

# The files that are in every checkout's top folder and together in no other.
LAUNCHER = "assisi"
TOOL_ENTRY = Path("tools") / "assisi" / "cli.py"

# Set by the Windows command, which cannot be a link, to say the tool was
# started as the installed command.
COMMAND_VARIABLE = "ASSISI_COMMAND"


def is_checkout(folder: Path) -> bool:
    return (folder / LAUNCHER).is_file() and (folder / TOOL_ENTRY).is_file()


def checkout_above(start: Path) -> Optional[Path]:
    """The checkout holding @p start, or None when it is in none."""
    for folder in (start, *start.parents):
        if is_checkout(folder):
            return folder
    return None


def via_command(script: Path, environment: Mapping[str, str]) -> bool:
    """Whether @p script was started as the installed command rather than by
    its path: the command is a link to it, or on Windows a file that says so."""
    return script.is_symlink() or environment.get(COMMAND_VARIABLE) == "1"


def handoff_target(own: Path, cwd: Path, command: bool) -> Optional[Path]:
    """The checkout to run instead of @p own, or None to run @p own: only the
    installed @p command hands off, and only to another checkout around @p cwd."""
    if not command:
        return None
    found = checkout_above(cwd)
    return found if found is not None and found != own else None
