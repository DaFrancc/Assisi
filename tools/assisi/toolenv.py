"""The TUI's own Python environment, under out/tool-env/.

The TUI is built on Textual, which the command line never needs. Installing it
into the system's Python is refused outright on distributions that mark theirs
as externally managed, and a distribution's own package lags far enough behind
that the TUI would run against whichever major version a machine happens to
ship. So it lives in a virtual environment the tool creates on first use, from
pinned and hashed requirements, and the tool restarts itself inside it to open
the TUI. The command line itself stays on the standard library and runs on the
Steam Runtime container's Python 3.9.
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
import venv
from pathlib import Path
from typing import Optional, Tuple

from assisi.project import Project

# markdown-it-py and platformdirs, which Textual needs, require 3.10.
TUI_MIN_PYTHON = (3, 10)

ENV_DIR = "tool-env"
REQUIREMENTS = Path(__file__).resolve().parent / "tui" / "requirements.txt"

# Written into the environment once its install succeeds, holding the digest of
# the requirements it was made from. An environment without it, or with another
# digest, is rebuilt: half an install or an outdated one is never used.
STAMP = "assisi-requirements.sha256"


def env_dir(project: Project) -> Path:
    return project.out_dir() / ENV_DIR


def env_python(project: Project) -> Path:
    if project.host == "Windows":
        return env_dir(project) / "Scripts" / "python.exe"
    return env_dir(project) / "bin" / "python"


def requirements_digest() -> str:
    return hashlib.sha256(REQUIREMENTS.read_bytes()).hexdigest()


def stamp_path(project: Project) -> Path:
    return env_dir(project) / STAMP


def is_ready(project: Project) -> bool:
    try:
        stamped = stamp_path(project).read_text().strip()
    except OSError:
        return False
    return env_python(project).exists() and stamped == requirements_digest()


def describe(project: Project) -> Tuple[bool, str]:
    """For doctor: whether the environment is usable, and what state it is in."""
    if is_ready(project):
        return True, f"ready in {env_dir(project).relative_to(project.root)}"
    if env_dir(project).exists():
        return False, "out of date; ./assisi tui --reinstall rebuilds it"
    return False, "not created yet"


def python_too_old(version: Tuple[int, ...]) -> Optional[str]:
    if tuple(version[:2]) < TUI_MIN_PYTHON:
        wanted = ".".join(map(str, TUI_MIN_PYTHON))
        return (f"the TUI needs Python {wanted} or newer and this is {'.'.join(map(str, version[:3]))}. "
                "Every command still works from the command line.")
    return None


def create(project: Project, version: Tuple[int, ...] = tuple(sys.version_info)) -> Optional[str]:
    """Build the environment from the pinned requirements. Returns why it could
    not be built, or None once it is ready."""
    reason = python_too_old(version)
    if reason:
        return reason
    target = env_dir(project)
    if target.exists():
        shutil.rmtree(target)
    print(f"assisi: setting up the TUI in {target.relative_to(project.root)} (once; it needs the network)",
          flush=True)
    try:
        venv.EnvBuilder(with_pip=True, clear=True).create(target)
    except (OSError, subprocess.CalledProcessError) as error:
        shutil.rmtree(target, ignore_errors=True)
        return (f"Python could not create a virtual environment ({error}). On Debian and Ubuntu, "
                "install python3-venv and try again.")
    code = subprocess.call([str(env_python(project)), "-m", "pip", "install", "--quiet",
                            "--disable-pip-version-check", "--require-hashes", "-r", str(REQUIREMENTS)])
    if code != 0:
        shutil.rmtree(target, ignore_errors=True)
        return ("pip could not install the TUI's packages. Check the network connection, then run "
                "./assisi tui --reinstall.")
    stamp_path(project).write_text(requirements_digest() + "\n")
    return None


def start(project: Project, reinstall: bool = False) -> Optional[int]:
    """Open the TUI, creating its environment first if it needs it. Returns the
    TUI's exit code, or None when it could not be started (the reason printed)."""
    if reinstall or not is_ready(project):
        reason = create(project)
        if reason:
            print(f"assisi: {reason}", file=sys.stderr)
            return None
    # A separate process rather than exec, which Windows does not do in place.
    return subprocess.call([str(env_python(project)), str(project.launcher()), "tui", "--in-env"],
                           cwd=project.root)
