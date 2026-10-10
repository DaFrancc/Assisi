"""Everything the tool knows about the repository's builds.

A build is a CMake preset and its tree under out/build/, named for its compiler
and level together (gcc-debug). People never type those names: they give a
level (debug, dev, ship) and, when it is not their default, a compiler, plus
--profiler, --sanitize or --steam-runtime for the special builds. tree_for
turns that into the preset, and refuses a combination no preset exists for.

The presets are listed here rather than read from CMakePresets.json at run time
so that a preset with no entry here is a test failure instead of a build the
tool silently cannot reach; the tests compare the two lists in both directions.
"""

from __future__ import annotations

import enum
import json
import os
import platform
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence

COMPILERS = ("msvc", "gcc", "clang")
CONFIGS = ("debug", "dev", "ship")

# Appended to a preset name for its twin with the capture system compiled in.
CHIARA_SUFFIX = "-chiara"

# Each inherits its compiler's debug preset, and the last one adds Chiara as well,
# because Chiara's own race tests need both.
SANITIZED = ("msvc-asan", "gcc-asan", "gcc-tsan", "clang-asan", "clang-tsan", "gcc-tsan-chiara")
SANITIZED_CONFIG = "debug"

# What --sanitize takes, and the part of the preset name each one is.
SANITIZERS = ("address", "thread")
SANITIZER_PART = {"address": "asan", "thread": "tsan"}

# gcc-ship built inside Valve's Steam Runtime SDK container. The preset only
# exists inside the container; on the host the tool runs itself in there.
STEAMRT_BUILD = "gcc-ship-steamrt"
STEAMRT_COMPILER = "gcc"
STEAMRT_CONFIG = "ship"
STEAMRT_ENVIRONMENT = "ASSISI_STEAMRT_SDK"

# The texture compression tier a cook uses, and the codec a pak is compressed
# with, per config. A debug cook takes the fast tier because somebody is waiting
# on it, so a debug pak is never a packaging input.
TIERS = {"debug": "fast", "dev": "best", "ship": "best"}
CODECS = {"debug": "none", "dev": "lz4", "ship": "zstd"}

# Which compilers a host can build with, and which one a bare config name means.
HOST_COMPILERS = {"Linux": ("gcc", "clang"), "Windows": ("msvc",)}
DEFAULT_COMPILER = {"Linux": "gcc", "Windows": "msvc"}

# The config a build is when nobody has chosen a default.
DEFAULT_CONFIG = "dev"

OUT_DIR = "out"
BUILD_DIR = "build"
DEPS_DIR = "_deps-src"
STEAMRT_HOME_DIR = "steamrt-home"
CONFIGURE_SCRIPT = ("cmake", "AssisiConfigureCached.cmake")
CACHE_FILE = "CMakeCache.txt"

EDITOR = "Assisi-GameEditor"
GAME = "Assisi-Game"
COOK_TOOL = ("cook", "assisi-cook")
PACK_TOOL = ("pack", "assisi-pack")
GAME_DIR = ("apps", "game")
COOKED_DIR = "cooked"
PAK_NAME = "assets.pak"

# Where releases are kept: at the root rather than under out/, so that cleaning
# every build never deletes what players have.
RELEASES_DIR = "releases"

# The per-developer settings file, kept with the history and recipes, and the
# keys of the defaults it holds.
SETTINGS_DIR = ".assisi"
SETTINGS_FILE = "config.json"
COMPILER_KEY = "compiler"
LEVEL_KEY = "level"


class UsageError(Exception):
    """A request the tool refuses before running anything, with the reason."""


class Status(enum.Enum):
    MISSING = "missing"
    BUILT = "built"
    STALE = "stale"


@dataclass(frozen=True)
class BuildSpec:
    """A build as people name it: a level and what it is built with."""

    compiler: str
    level: str
    profiler: bool = False
    sanitize: str | None = None
    steam_runtime: bool = False

    def extras(self) -> str:
        """The flags beyond level and compiler, as words for a table."""
        words = []
        if self.profiler:
            words.append("profiler")
        if self.sanitize:
            words.append(f"{self.sanitize} sanitizer")
        if self.steam_runtime:
            words.append("Steam Runtime")
        return ", ".join(words)

    def tags(self) -> str:
        """The same as extras, in the short form a table column has room for."""
        words = []
        if self.profiler:
            words.append("profiler")
        if self.sanitize:
            words.append(SANITIZER_PART[self.sanitize])
        if self.steam_runtime:
            words.append("steam runtime")
        return " ".join(words)

    def argv(self) -> list:
        """The command-line words that name this build."""
        words = [self.level, "--compiler", self.compiler]
        if self.profiler:
            words.append("--profiler")
        if self.sanitize:
            words += ["--sanitize", self.sanitize]
        if self.steam_runtime:
            words.append("--steam-runtime")
        return words


@dataclass(frozen=True)
class BuildStatus:
    """What exists for one build, read from the files on disk."""

    configured: Status
    editor: Status
    game: Status
    cooked: Status
    packed: Status


def base_presets(compilers: Sequence[str] = COMPILERS) -> list[str]:
    return [f"{compiler}-{config}" for compiler in compilers for config in CONFIGS]


def all_trees() -> list[str]:
    """Every build on every host, in a stable order."""
    plain = base_presets()
    return plain + [name + CHIARA_SUFFIX for name in plain] + list(SANITIZED) + [STEAMRT_BUILD]


def compiler_of(tree: str) -> str:
    return tree.split("-", 1)[0]


def spec_of(tree: str) -> BuildSpec:
    """The level, compiler and flags a preset is."""
    if tree == STEAMRT_BUILD:
        return BuildSpec(STEAMRT_COMPILER, STEAMRT_CONFIG, steam_runtime=True)
    profiler = tree.endswith(CHIARA_SUFFIX)
    stem = tree[:-len(CHIARA_SUFFIX)] if profiler else tree
    compiler, part = stem.split("-", 1)
    sanitizer = next((name for name, value in SANITIZER_PART.items() if value == part), None)
    if sanitizer is not None:
        return BuildSpec(compiler, SANITIZED_CONFIG, profiler=profiler, sanitize=sanitizer)
    return BuildSpec(compiler, part, profiler=profiler)


def config_of(tree: str) -> str:
    return spec_of(tree).level


class Project:
    """One repository on one host."""

    def __init__(self, root: Path, host: str | None = None, environment: Mapping[str, str] | None = None):
        self.root = Path(root)
        self.host = host if host is not None else platform.system()
        self.environment = environment if environment is not None else os.environ

    # --- Names

    def compilers(self) -> tuple[str, ...]:
        return HOST_COMPILERS.get(self.host, ())

    def trees(self) -> list[str]:
        """Every build this host can name, in the order the TUI lists them."""
        return [tree for tree in all_trees() if compiler_of(tree) in self.compilers()]

    def trees_of_compiler(self, compiler: str) -> list[str]:
        """Everything a compiler owns, for clean: its plain, Chiara and sanitizer
        builds, and for gcc the Steam Runtime build as well."""
        return [tree for tree in all_trees() if compiler_of(tree) == compiler]

    def tree_for(self, spec: BuildSpec) -> str:
        """The preset @p spec names, or a UsageError saying why there is none."""
        if spec.compiler not in self.compilers():
            raise UsageError(f"{spec.compiler} builds need {self.host_of(spec.compiler)}; this is {self.host}. "
                             f"Compilers here: {', '.join(self.compilers())}.")
        if spec.level not in CONFIGS:
            raise UsageError(f"'{spec.level}' is not a level. Levels: {', '.join(CONFIGS)}.")
        if spec.steam_runtime:
            if spec.profiler or spec.sanitize:
                raise UsageError("--steam-runtime is a plain release build; it takes no --profiler or --sanitize.")
            if (spec.compiler, spec.level) != (STEAMRT_COMPILER, STEAMRT_CONFIG):
                raise UsageError(f"the Steam Runtime build is {STEAMRT_CONFIG} with {STEAMRT_COMPILER}: "
                                 f"{STEAMRT_CONFIG} --compiler {STEAMRT_COMPILER} --steam-runtime.")
            return STEAMRT_BUILD
        suffix = CHIARA_SUFFIX if spec.profiler else ""
        if spec.sanitize:
            if spec.level != SANITIZED_CONFIG:
                raise UsageError(f"sanitizer builds are {SANITIZED_CONFIG} builds: "
                                 f"{SANITIZED_CONFIG} --sanitize {spec.sanitize}.")
            tree = f"{spec.compiler}-{SANITIZER_PART[spec.sanitize]}{suffix}"
        else:
            tree = f"{spec.compiler}-{spec.level}{suffix}"
        if tree not in self.trees():
            raise UsageError(f"there is no {spec.level} build with {spec.compiler} and {spec.extras()}.")
        return tree

    @staticmethod
    def is_build_word(word: str) -> bool:
        """Whether @p word already means a build: a level, a compiler or a preset."""
        return word in CONFIGS or word in COMPILERS or word in all_trees()

    @staticmethod
    def host_of(compiler: str) -> str:
        return next((host for host, compilers in HOST_COMPILERS.items() if compiler in compilers), "another host")

    def default_compiler(self) -> str:
        """The developer's chosen compiler, or this host's usual one."""
        saved = self.read_settings().get(COMPILER_KEY)
        if isinstance(saved, str) and saved in self.compilers():
            return saved
        return DEFAULT_COMPILER.get(self.host, COMPILERS[1])

    def default_level(self) -> str:
        saved = self.read_settings().get(LEVEL_KEY)
        return saved if isinstance(saved, str) and saved in CONFIGS else DEFAULT_CONFIG

    def default_build(self) -> str:
        return self.tree_for(BuildSpec(self.default_compiler(), self.default_level()))

    def settings_path(self) -> Path:
        return self.root / SETTINGS_DIR / SETTINGS_FILE

    def read_settings(self) -> dict:
        try:
            value = json.loads(self.settings_path().read_text())
        except (OSError, ValueError):
            return {}
        return value if isinstance(value, dict) else {}

    def write_settings(self, settings: dict) -> None:
        self.settings_path().parent.mkdir(parents=True, exist_ok=True)
        self.settings_path().write_text(json.dumps(settings, indent=2) + "\n")

    # --- Paths

    def executable(self, path: Path) -> Path:
        return path.with_name(path.name + ".exe") if self.host == "Windows" else path

    def out_dir(self) -> Path:
        return self.root / OUT_DIR

    def build_dir(self, tree: str) -> Path:
        return self.out_dir() / BUILD_DIR / tree

    def deps_dir(self) -> Path:
        return self.out_dir() / DEPS_DIR

    def steamrt_home(self) -> Path:
        return self.out_dir() / STEAMRT_HOME_DIR

    def configure_script(self) -> Path:
        return self.root.joinpath(*CONFIGURE_SCRIPT)

    def game_dir(self, tree: str) -> Path:
        return self.build_dir(tree).joinpath(*GAME_DIR)

    def editor_path(self, tree: str) -> Path:
        return self.executable(self.game_dir(tree) / EDITOR)

    def game_path(self, tree: str) -> Path:
        return self.executable(self.game_dir(tree) / GAME)

    def cook_tool(self, tree: str) -> Path:
        return self.executable(self.build_dir(tree).joinpath("apps", *COOK_TOOL))

    def pack_tool(self, tree: str) -> Path:
        return self.executable(self.build_dir(tree).joinpath("apps", *PACK_TOOL))

    def cooked_dir(self, tree: str) -> Path:
        return self.build_dir(tree) / COOKED_DIR

    def pak_path(self, tree: str) -> Path:
        return self.game_dir(tree) / PAK_NAME

    def assets_dir(self) -> Path:
        return self.root / "assets"

    def releases_dir(self) -> Path:
        return self.root / RELEASES_DIR

    def path_from(self, given: str) -> Path:
        """A path as typed on the command line, which runs from the repository root."""
        path = Path(given)
        return path if path.is_absolute() else self.root / path

    def launcher(self) -> Path:
        return self.root / "assisi"

    def steamrt_helper(self) -> Path:
        return self.root / "scripts" / "steamrt.py"

    # --- State

    def in_steamrt_container(self) -> bool:
        return bool(self.environment.get(STEAMRT_ENVIRONMENT))

    def is_configured(self, tree: str) -> bool:
        return (self.build_dir(tree) / CACHE_FILE).is_file()

    def build_status(self, tree: str) -> BuildStatus:
        def exists(path: Path) -> Status:
            return Status.BUILT if path.exists() else Status.MISSING

        cooked = self.cooked_dir(tree)
        cooked_status = Status.BUILT if cooked.is_dir() and any(cooked.iterdir()) else Status.MISSING
        pak = self.pak_path(tree)
        packed = exists(pak)
        if packed is Status.BUILT and cooked_status is Status.BUILT:
            newest = max((path.stat().st_mtime for path in cooked.rglob("*") if path.is_file()), default=0.0)
            if pak.stat().st_mtime < newest:
                packed = Status.STALE
        return BuildStatus(configured=Status.BUILT if self.is_configured(tree) else Status.MISSING,
                           editor=exists(self.editor_path(tree)), game=exists(self.game_path(tree)),
                           cooked=cooked_status, packed=packed)
