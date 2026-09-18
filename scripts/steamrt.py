#!/usr/bin/env python3
"""Build and check the game inside Valve's Steam Linux Runtime SDK.

A program runs on the glibc it was built against or newer, never older, so a
game built on a current distro refuses to start on anything older. Built in the
SDK's container, which carries the glibc Steam's own runtime does, it starts on
any distro at least that new and under Steam.

The whole thing is optional. Nothing else in the build calls this; the Makefile's
steamrt targets are the only way in, and a machine with no container runtime
never touches it.

  steamrt.py prepare             find podman or docker, pull the SDK, build the image
  steamrt.py run -- <command>    run <command> in the image, in this repository
  steamrt.py boot-check          boot the staged game in a bare older distro
  steamrt.py remove              delete every image this pulled or built

Exit codes: 0 done, 1 failed, SKIP_EXIT_CODE a boot check with nothing to boot
or nothing to boot it in.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

# The same skip code the dependency and symbol checks use, for the same reason:
# a check that could not run is neither a pass nor a failure.
SKIP_EXIT_CODE = 77

# Steam Runtime 3 "sniper", build 3.0.20260805.254768: Debian 11, glibc 2.31.
# Pinned by digest so a release built today and one built next year are built
# by the same compiler against the same libraries.
SDK_IMAGE = ("registry.gitlab.steamos.cloud/steamrt/sniper/sdk"
             "@sha256:1c33c507bc75d012e77df5727f93b0d5b8c3f7c8d4142ba5f7a16882cc92e014")
SDK_DOWNLOAD_SIZE = "3.9 GB"

# A bare Debian 11 (glibc 2.31) with no development packages: what the oldest
# player machine the game promises to support looks like.
CLEAN_SYSTEM_IMAGE = ("docker.io/library/debian"
                      "@sha256:e5b6442dd2e9684cf5e87d8338b5968f3b348636fc0be6d7850a381e3731a2bd")

# The SDK plus what the build needs on top of it. Tagged with a hash of its
# definition, so an edit to the Containerfile or a new SDK pin builds a fresh
# image and an unchanged one is reused.
DERIVED_IMAGE_NAME = "assisi-steamrt"
DERIVED_TAG_LENGTH = 12

# Preferred first. Rootless podman maps the developer's own user into the
# container; docker is told to run as that user instead.
RUNTIMES = ("podman", "docker")

# A headless boot of a thin package takes well under a second; this only stops
# a hung game from hanging the check.
BOOT_TIMEOUT_SECONDS = 60

REPO = Path(__file__).resolve().parent.parent
CONTAINERFILE_DIR = REPO / "scripts" / "steamrt"
STAGED_GAME_DIR = REPO / "out" / "build" / "gcc-ship-steamrt" / "apps" / "game" / "staged"
GAME_NAME = "Assisi-Game"

# HOME inside the container. Neither runtime gives the mapped user a home that
# exists there, and ccache and git both want one; under out/ it is per checkout
# and owned by the developer.
CONTAINER_HOME = REPO / "out" / "steamrt-home"


def fail(message: str) -> None:
    print(f"steamrt: {message}", file=sys.stderr)
    sys.exit(1)


def find_runtime() -> str:
    for runtime in RUNTIMES:
        if shutil.which(runtime):
            return runtime
    fail("the Steam Runtime build needs podman or docker, and neither is installed. "
         "Install either one; podman runs without root.")
    return ""


def call(runtime: str, *arguments: str, quiet: bool = False) -> int:
    output = subprocess.DEVNULL if quiet else None
    return subprocess.run([runtime, *arguments], stdout=output, stderr=output, check=False).returncode


def image_present(runtime: str, image: str) -> bool:
    return call(runtime, "image", "inspect", image, quiet=True) == 0


def ensure_pulled(runtime: str, image: str, what: str) -> None:
    if image_present(runtime, image):
        return
    print(f"steamrt: downloading {what} (once; `make steamrt-remove` deletes it)")
    if call(runtime, "pull", image) != 0:
        fail(f"could not download {image}")


def derived_image() -> str:
    definition = (CONTAINERFILE_DIR / "Containerfile").read_bytes() + SDK_IMAGE.encode()
    return f"{DERIVED_IMAGE_NAME}:{hashlib.sha256(definition).hexdigest()[:DERIVED_TAG_LENGTH]}"


def prepare() -> tuple[str, str]:
    """The runtime to use and the build image, pulled and built as needed."""
    runtime = find_runtime()
    ensure_pulled(runtime, SDK_IMAGE, f"the Steam Runtime SDK, about {SDK_DOWNLOAD_SIZE}")
    image = derived_image()
    if not image_present(runtime, image):
        print(f"steamrt: building {image} on top of the SDK")
        # The context is the Containerfile's own directory, never the
        # repository: docker would otherwise upload every build tree to itself.
        if call(runtime, "build", "-f", str(CONTAINERFILE_DIR / "Containerfile"), "-t", image,
                "--build-arg", f"BASE={SDK_IMAGE}", str(CONTAINERFILE_DIR)) != 0:
            fail(f"could not build {image}")
    return runtime, image


def user_arguments(runtime: str) -> list[str]:
    """Run as the developer, so nothing written to the tree is owned by root."""
    if runtime == "podman":
        return ["--userns=keep-id"]
    return ["--user", f"{os.getuid()}:{os.getgid()}"]


def run(command: list[str]) -> int:
    if not command:
        fail("run needs a command after --")
    runtime, image = prepare()
    CONTAINER_HOME.mkdir(parents=True, exist_ok=True)
    arguments = ["run", "--rm",
                 # Mounting the tree needs no SELinux relabel of the developer's files.
                 "--security-opt", "label=disable",
                 *user_arguments(runtime),
                 # At its own path: every configured tree and the shared
                 # dependency cache record absolute paths into the repository.
                 "-v", f"{REPO}:{REPO}", "-w", str(REPO),
                 "-e", f"HOME={CONTAINER_HOME}"]
    if sys.stdin.isatty():
        arguments += ["-it"]
    return call(runtime, *arguments, image, *command)


def boot_check(staged: Path) -> int:
    game = staged / GAME_NAME
    if not game.is_file():
        print(f"steamrt: no {game} to boot. `make gs-steamrt-test` builds and stages it first.",
              file=sys.stderr)
        return SKIP_EXIT_CODE
    runtime = next((candidate for candidate in RUNTIMES if shutil.which(candidate)), None)
    if runtime is None:
        print("steamrt: the boot check needs podman or docker, and neither is installed.", file=sys.stderr)
        return SKIP_EXIT_CODE

    ensure_pulled(runtime, CLEAN_SYSTEM_IMAGE, "a bare Debian 11 to boot the game in, about 30 MB")
    # Read-only and offline, with nothing but the game and its package: the game
    # gets no chance to lean on anything a player's machine would not have.
    code = call(runtime, "run", "--rm", "--security-opt", "label=disable", "--network", "none",
                "-v", f"{staged}:/game:ro", "-w", "/game", CLEAN_SYSTEM_IMAGE,
                "timeout", str(BOOT_TIMEOUT_SECONDS), f"/game/{GAME_NAME}", "--headless", "--ticks", "1")
    if code != 0:
        print(f"steamrt: {game} did not boot on a bare Debian 11 (exit {code})", file=sys.stderr)
        return 1
    print(f"steamrt: {game} boots on a bare Debian 11")
    return 0


def remove() -> int:
    runtime = find_runtime()
    listing = subprocess.run([runtime, "images", "--format", "{{.Repository}}:{{.Tag}}"],
                             capture_output=True, text=True, check=False)
    derived = [line for line in listing.stdout.split()
               if line.split("/")[-1].startswith(f"{DERIVED_IMAGE_NAME}:")]
    for image in [*derived, SDK_IMAGE, CLEAN_SYSTEM_IMAGE]:
        if image_present(runtime, image):
            print(f"steamrt: removing {image}")
            call(runtime, "rmi", image)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("prepare")
    run_parser = commands.add_parser("run")
    run_parser.add_argument("container_command", nargs=argparse.REMAINDER)
    boot_parser = commands.add_parser("boot-check")
    boot_parser.add_argument("--staged", type=Path, default=STAGED_GAME_DIR,
                             help="the directory holding the game and its assets.pak")
    commands.add_parser("remove")
    args = parser.parse_args()

    if not sys.platform.startswith("linux"):
        fail("the Steam Runtime build is for Linux hosts only")
    # A runtime's -v takes source:destination, so a colon in the path cannot be mounted.
    if ":" in str(REPO):
        fail(f"the repository path {REPO} contains ':', which a container mount cannot express")

    if args.command == "prepare":
        prepare()
        return 0
    if args.command == "run":
        command = args.container_command
        if command and command[0] == "--":
            command = command[1:]
        return run(command)
    if args.command == "boot-check":
        return boot_check(args.staged.resolve())
    return remove()


if __name__ == "__main__":
    sys.exit(main())
