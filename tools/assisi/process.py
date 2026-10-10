"""The one place the tool starts another program.

Every program runs from the repository root: `cmake --build --preset` and
`ctest --preset` look for CMakePresets.json in the working directory, so a call
made from anywhere else fails with a preset that "does not exist".
"""

from __future__ import annotations

import shlex
import subprocess
import sys
from pathlib import Path
from typing import Sequence


class CommandFailed(Exception):
    """A program exited non-zero. The tool stops and exits with the same code."""

    def __init__(self, code: int):
        super().__init__(f"exit {code}")
        self.code = code


def describe(argv: Sequence[str]) -> str:
    return " ".join(shlex.quote(str(part)) for part in argv)


class Runner:
    def __init__(self, root: Path):
        self.root = root

    def run(self, argv: Sequence[object]) -> None:
        """Echo @p argv, run it with the terminal attached, and raise CommandFailed
        on a non-zero exit. Output is not captured, so compilers keep their colour
        and ninja its progress line."""
        parts = [str(part) for part in argv]
        print(f"$ {describe(parts)}", flush=True)
        try:
            code = subprocess.call(parts, cwd=self.root)
        except FileNotFoundError as missing:
            print(f"assisi: {parts[0]} is not installed or not on PATH. `./assisi doctor` checks the setup.",
                  file=sys.stderr)
            raise CommandFailed(127) from missing
        if code != 0:
            raise CommandFailed(code)

    def capture(self, argv: Sequence[object]) -> str:
        """Run @p argv quietly and return its standard output, or raise CommandFailed."""
        parts = [str(part) for part in argv]
        try:
            result = subprocess.run(parts, cwd=self.root, capture_output=True, text=True, check=False)
        except FileNotFoundError as missing:
            raise CommandFailed(127) from missing
        if result.returncode != 0:
            raise CommandFailed(result.returncode)
        return result.stdout
