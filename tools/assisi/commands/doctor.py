"""doctor: check that this machine has what building the engine needs."""

from __future__ import annotations

import os
import re
import shutil
import sys
from dataclasses import dataclass
from typing import Callable, List, Optional, Tuple

from assisi.process import CommandFailed

NAME = "doctor"
HELP = "Check this machine has the tools building the engine needs, and say how to fix what is missing"
OPTIONS = ()
RECORDED = False

# The oldest CMake the top-level CMakeLists.txt accepts.
MINIMUM_CMAKE = (3, 28)
# The oldest Python the command line runs on: the Steam Runtime container's.
MINIMUM_PYTHON = (3, 9)

INSTALL_HELP = "see book/src/installation.md for your system"

STATIC_RUNTIME = "libstdc++.a"


@dataclass
class Check:
    name: str
    required: bool
    # Returns (passed, what was found or what is wrong).
    probe: Callable[[object], Tuple[bool, str]]
    remedy: str


def version_of(text: str) -> Optional[Tuple[int, ...]]:
    found = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", text)
    return tuple(int(part) for part in found.groups() if part is not None) if found else None


def on_path(program: str) -> Callable[[object], Tuple[bool, str]]:
    def probe(context) -> Tuple[bool, str]:
        found = shutil.which(program)
        return (found is not None, found or "not found")
    return probe


def probe_cmake(context) -> Tuple[bool, str]:
    if shutil.which("cmake") is None:
        return False, "not found"
    try:
        version = version_of(context.runner.capture(["cmake", "--version"]))
    except CommandFailed:
        return False, "would not report its version"
    if version is None or version[:2] < MINIMUM_CMAKE:
        return False, f"version {'.'.join(map(str, version or ()))} is older than {'.'.join(map(str, MINIMUM_CMAKE))}"
    return True, "version " + ".".join(map(str, version))


def probe_python(context) -> Tuple[bool, str]:
    current = sys.version_info[:3]
    shown = ".".join(map(str, current))
    return current[:2] >= MINIMUM_PYTHON, f"version {shown}"


def probe_static_runtime(context) -> Tuple[bool, str]:
    # The compiler answers with the bare name when it has no such file, and with
    # its full path when it does.
    if shutil.which("g++") is None:
        return False, "no g++ to ask"
    try:
        answer = context.runner.capture(["g++", f"-print-file-name={STATIC_RUNTIME}"]).strip()
    except CommandFailed:
        return False, "g++ would not answer"
    return (os.path.isabs(answer), answer if os.path.isabs(answer) else f"no {STATIC_RUNTIME}")


def probe_container_runtime(context) -> Tuple[bool, str]:
    found = shutil.which("podman") or shutil.which("docker")
    return (found is not None, found or "neither podman nor docker")


def probe_tui(context) -> Tuple[bool, str]:
    from assisi import toolenv
    return toolenv.describe(context.project)


def checks(host: str) -> List[Check]:
    rows = [
        Check("python", True, probe_python, f"install Python {'.'.join(map(str, MINIMUM_PYTHON))} or newer"),
        Check("cmake", True, probe_cmake, f"install CMake {'.'.join(map(str, MINIMUM_CMAKE))} or newer; "
              + INSTALL_HELP),
        Check("ninja", True, on_path("ninja"), "install Ninja; " + INSTALL_HELP),
        Check("ccache", True, on_path("ccache"), "install ccache, which every preset compiles through; "
              + INSTALL_HELP),
        Check("git", True, on_path("git"), "install Git; " + INSTALL_HELP),
    ]
    if host == "Windows":
        rows.append(Check("cl", True, on_path("cl"), "run from the Developer Command Prompt for Visual Studio"))
    else:
        rows += [
            Check("g++", True, on_path("g++"), "install GCC with C++ support; " + INSTALL_HELP),
            Check("static libstdc++", True, probe_static_runtime,
                  "install the static C++ runtime (on Fedora: libstdc++-static); a Release game links it in"),
            Check("clang++", False, on_path("clang++"), "only needed for the clang builds"),
            Check("podman or docker", False, probe_container_runtime,
                  "only needed for the Steam Runtime build; see book/src/steam-runtime.md"),
        ]
    rows += [
        Check("uncrustify", False, on_path("uncrustify"), "only needed for ./assisi format"),
        Check("TUI environment", False, probe_tui, "created the first time ./assisi opens the TUI"),
    ]
    return rows


def execute(request, context) -> int:
    failed = False
    for check in checks(context.project.host):
        passed, detail = check.probe(context)
        if passed:
            mark = "ok      "
        elif check.required:
            mark = "MISSING "
            failed = True
        else:
            mark = "optional"
        print(f"  {mark}  {check.name:<18} {detail}")
        if not passed:
            print(f"            {'':<18} -> {check.remedy}")
    print("All required tools are present." if not failed else "Some required tools are missing.")
    return 1 if failed else 0
