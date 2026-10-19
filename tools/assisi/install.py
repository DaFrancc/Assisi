"""The `assisi` command: the tool, typed by name from any folder.

On Linux and macOS it is a link in ~/.local/bin to a checkout's assisi script;
on Windows, a small assisi.cmd in a folder of the user's own that is put on
their PATH. Either way it names one checkout, and installing for another asks
first. Nothing here edits a shell's profile: when the folder is not on PATH,
the developer is told the line to add.

Every folder comes from the environment handed in, never from the real home,
so the tests install into a scratch folder.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import Callable, Mapping, Optional

from assisi import locate

Confirm = Callable[[str], bool]
Say = Callable[[str], None]

COMMAND_NAME = "assisi"

# Where the command goes under the home folder on Linux and macOS, and under
# %LOCALAPPDATA% on Windows.
POSIX_BIN = Path(".local") / "bin"
WINDOWS_FOLDER = Path("Programs") / "Assisi"
WINDOWS_COMMAND = "assisi.cmd"

# The line of a Windows command that records which checkout it runs.
CHECKOUT_LINE = "@rem checkout: "

# Where the line that puts a folder on PATH goes, by the shell's name, and how
# that shell says it.
SHELL_PROFILES = {"zsh": "~/.zshrc", "bash": "~/.bashrc", "fish": "~/.config/fish/config.fish"}
FISH = "fish"

# The user's PATH in the registry, and the message that tells running programs
# it changed, so a new terminal sees it without signing out.
ENVIRONMENT_KEY = "Environment"
PATH_VALUE = "Path"
WM_SETTINGCHANGE = 0x001A
HWND_BROADCAST = 0xFFFF
SMTO_ABORTIFHUNG = 0x0002
BROADCAST_TIMEOUT_MS = 5000


# --- Linux and macOS

def install_posix(checkout: Path, bin_dir: Path, confirm: Confirm, say: Say) -> bool:
    """Link @p bin_dir/assisi to @p checkout's script. A link to another
    checkout is replaced only when @p confirm agrees; anything else already
    there is left alone. Returns whether the command now runs @p checkout."""
    target = checkout / locate.LAUNCHER
    command = bin_dir / COMMAND_NAME
    if command.is_symlink():
        current = command.resolve()
        if current == target.resolve():
            say(f"`assisi` already runs this checkout ({command}).")
            return True
        if not confirm(f"`assisi` runs {current.parent} now. Make it run this checkout instead?"):
            return False
        command.unlink()
    elif command.exists():
        say(f"{command} is already there and is not a link this tool made, so it is left alone.")
        return False
    bin_dir.mkdir(parents=True, exist_ok=True)
    command.symlink_to(target)
    say(f"`assisi` now runs this checkout, linked from {command}.")
    return True


def on_path(folder: Path, path_value: str, separator: str = os.pathsep) -> bool:
    entries = [entry.rstrip("/\\") for entry in path_value.split(separator) if entry]
    return str(folder).rstrip("/\\") in entries


def path_advice(bin_dir: Path, path_value: str, shell: str) -> Optional[str]:
    """What to add to the shell's profile so @p bin_dir is on PATH, or None
    when it already is."""
    if on_path(bin_dir, path_value):
        return None
    name = Path(shell).name if shell else ""
    line = f"fish_add_path {bin_dir}" if name == FISH else f'export PATH="{bin_dir}:$PATH"'
    profile = SHELL_PROFILES.get(name, "your shell's profile")
    return f"{bin_dir} is not on your PATH yet. Add this line to {profile}, then open a new terminal:\n    {line}"


# --- Windows

def cmd_text(checkout: Path, python: Path) -> str:
    """The Windows command: run @p checkout's tool with @p python, saying it
    was started as the command so the tool hands off to the checkout it is in."""
    return "\r\n".join(["@echo off", f"{CHECKOUT_LINE}{checkout}", "setlocal",
                        f'set "{locate.COMMAND_VARIABLE}=1"',
                        f'"{python}" "{checkout / locate.LAUNCHER}" %*', ""])


def cmd_checkout(text: str) -> Optional[str]:
    """The checkout a Windows command runs, read back from its text."""
    for line in text.splitlines():
        if line.startswith(CHECKOUT_LINE):
            return line[len(CHECKOUT_LINE):].strip()
    return None


def path_with(path_value: str, entry: str) -> Optional[str]:
    """A Windows PATH with @p entry added at its end, or None when it is there
    already; Windows paths are compared without case."""
    entries = [part.rstrip("\\").lower() for part in path_value.split(";") if part]
    if entry.rstrip("\\").lower() in entries:
        return None
    return f"{path_value.rstrip(';')};{entry}" if path_value else entry


def append_user_path(entry: str) -> bool:
    """Add @p entry to the user's PATH in the registry and tell running
    programs. Returns whether it was added rather than already there."""
    import ctypes
    import winreg

    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, ENVIRONMENT_KEY, 0,
                        winreg.KEY_READ | winreg.KEY_WRITE) as key:
        try:
            value, kind = winreg.QueryValueEx(key, PATH_VALUE)
        except FileNotFoundError:
            value, kind = "", winreg.REG_EXPAND_SZ
        updated = path_with(value, entry)
        if updated is None:
            return False
        winreg.SetValueEx(key, PATH_VALUE, 0, kind, updated)
    result = ctypes.c_size_t()
    ctypes.windll.user32.SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, ENVIRONMENT_KEY,
                                             SMTO_ABORTIFHUNG, BROADCAST_TIMEOUT_MS, ctypes.byref(result))
    return True


def install_windows(checkout: Path, folder: Path, python: Path, confirm: Confirm, say: Say) -> bool:
    """Write @p folder/assisi.cmd for @p checkout and put @p folder on the
    user's PATH. A command for another checkout is replaced only when
    @p confirm agrees."""
    command = folder / WINDOWS_COMMAND
    if command.exists():
        current = cmd_checkout(command.read_text())
        if current == str(checkout):
            say(f"`assisi` already runs this checkout ({command}).")
            return True
        if current is None:
            say(f"{command} is already there and is not one this tool made, so it is left alone.")
            return False
        if not confirm(f"`assisi` runs {current} now. Make it run this checkout instead?"):
            return False
    folder.mkdir(parents=True, exist_ok=True)
    command.write_text(cmd_text(checkout, python), newline="")
    if append_user_path(str(folder)):
        say(f"`assisi` now runs this checkout. {folder} was added to your PATH; open a new terminal to use it.")
    else:
        say(f"`assisi` now runs this checkout ({command}).")
    return True


# --- Either

def install(checkout: Path, host: str, environment: Mapping[str, str], confirm: Confirm, say: Say) -> bool:
    """Install the command for @p checkout on @p host, with its folders taken
    from @p environment. Says why and installs nothing when a folder it needs
    is not named there."""
    if host == "Windows":
        local = environment.get("LOCALAPPDATA")
        if not local:
            say("%LOCALAPPDATA% is not set, so the `assisi` command cannot be installed.")
            return False
        return install_windows(checkout, Path(local) / WINDOWS_FOLDER, Path(sys.executable), confirm, say)
    home = environment.get("HOME")
    if not home:
        say("$HOME is not set, so the `assisi` command cannot be installed.")
        return False
    bin_dir = Path(home) / POSIX_BIN
    installed = install_posix(checkout, bin_dir, confirm, say)
    advice = path_advice(bin_dir, environment.get("PATH", ""), environment.get("SHELL", ""))
    if installed and advice:
        say(advice)
    return installed
