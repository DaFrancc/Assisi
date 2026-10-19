"""First-time setup: check the machine, pick the defaults, install the command.

It runs by itself the first time the tool is used at a terminal in a
checkout, before whatever was asked for, and again whenever ./assisi setup is
run. Each question shows its default, which Enter keeps. Closing the input
ends setup there and leaves the rest as it was.
"""

from __future__ import annotations

from typing import Callable, Sequence

from assisi import install
from assisi.commands import doctor
from assisi.project import COMPILER_KEY, CONFIGS, LEVEL_KEY, SETUP_KEY, Project

Ask = Callable[[str], str]

YES = ("y", "yes")
NO = ("n", "no")


def ask_choice(ask: Ask, question: str, choices: Sequence[str], default: str) -> str:
    """One of @p choices, asked until the answer is one; Enter takes @p default."""
    while True:
        answer = ask(f"{question} [{'/'.join(choices)}] ({default}): ").strip().lower()
        if not answer:
            return default
        if answer in choices:
            return answer
        print(f"  Type one of {', '.join(choices)}, or press Enter for {default}.")


def ask_yes_no(ask: Ask, question: str) -> bool:
    """Yes or no, asked until the answer is one; Enter means yes."""
    while True:
        answer = ask(f"{question} [Y/n]: ").strip().lower()
        if not answer or answer in YES:
            return True
        if answer in NO:
            return False
        print("  Type y or n.")


def offered(project: Project) -> bool:
    return bool(project.read_settings().get(SETUP_KEY))


def mark_offered(project: Project) -> None:
    settings = project.read_settings()
    settings[SETUP_KEY] = True
    project.write_settings(settings)


def choose_defaults(project: Project, ask: Ask) -> None:
    """The default compiler, asked only when the host has more than one, and level."""
    compilers = project.compilers()
    compiler = project.default_compiler()
    if len(compilers) > 1:
        compiler = ask_choice(ask, "Default compiler", compilers, compiler)
    level = ask_choice(ask, "Default level", CONFIGS, project.default_level())
    settings = project.read_settings()
    settings[COMPILER_KEY] = compiler
    settings[LEVEL_KEY] = level
    project.write_settings(settings)
    print(f"  default: {level} with {compiler}")


def offer_command(project: Project, ask: Ask) -> None:
    def confirm(question: str) -> bool:
        return ask_yes_no(ask, f"  {question}")

    def say(line: str) -> None:
        print(f"  {line}")

    if ask_yes_no(ask, "Make `assisi` a command you can type in any folder?"):
        install.install(project.root, project.host, project.environment, confirm, say)


def read_answer(prompt: str) -> str:
    return input(prompt)


def run(context, ask: Ask = read_answer) -> int:
    """The three steps, each under a heading; returns 0 however it ends."""
    project = context.project
    print("\nAssisi first-time setup. ./assisi setup runs it again at any time.\n")
    try:
        print("1. The tools building the engine needs")
        doctor.report(context)
        print("\n2. Your defaults, used whenever a command names no level or compiler")
        choose_defaults(project, ask)
        print("\n3. The `assisi` command")
        offer_command(project, ask)
    except EOFError:
        print("\nSetup stopped; ./assisi setup picks it up again.")
    print()
    return 0
