# Building for every Linux distribution

On Linux there are two ways to build the game you give players. Both give you
the same two files, `Assisi-Game` and `assets.pak`. What differs is **which
Linux systems can run the game**.

| | Bare build | Steam Runtime build |
|---|---|---|
| Command | `make gsgkp` | `make gs-steamrt-game-cook-pack` |
| Built by | your own compiler, on your own system | the compiler in Valve's Steam Runtime SDK, inside a container |
| Runs on | your distribution, and others at least as new | any distribution from about 2020 on, and Steam Deck |
| Extra tools | none | podman or docker |
| Extra disk space | none | about 5 GB |
| Output folder | `out/build/gcc-ship/apps/game/` | `out/build/gcc-ship-steamrt/apps/game/` |

**Use the bare build** while you work, and to share builds with people on
systems like yours. **Use the Steam Runtime build** for anything you release
publicly, on Steam or anywhere else.

## Why the bare build doesn't run everywhere

Every Linux program uses the system's C library, **glibc**. A program runs on
the glibc it was built with or a newer one, never an older one. The bare build
uses the glibc of the machine you built it on. If that's a current rolling or
fast-moving distribution, the game won't start on Ubuntu LTS, Debian stable or
anything else older. The player sees an error like this and nothing else:

```text
./Assisi-Game: /lib/x86_64-linux-gnu/libc.so.6: version `GLIBC_2.38' not found
```

Running the game through Steam doesn't avoid this. Steam uses the player's own
glibc whenever it is newer than its runtime's, which it nearly always is.

The Steam Runtime build compiles the game inside Valve's **Steam Linux Runtime
3 ("sniper") SDK**, the environment Steam itself runs native Linux games in.
It's based on Debian 11, whose glibc is 2.31, so the game needs glibc 2.31 or
newer. Ubuntu 20.04, Debian 11, Fedora 32 and every later release have it.

**The container is only for building.** The finished game is an ordinary
program. Players double-click it or launch it from Steam like any other game.
They don't need podman, docker or anything else.

## What you need

On top of everything from [Installation](installation.md):

- **podman or docker.** Either works; podman is preferred when both are
  installed, because it runs without root.
- **About 5 GB of disk space**: 4.3 GB for the SDK and a small image built on
  top of it, and about 1 GB for the build and its compiler cache.
- **An internet connection the first time**, to download the SDK. Later builds
  work offline.

You don't install anything else. The SDK brings its own compiler, CMake and
libraries, and the build uses the same dependency downloads as your other
builds.

### Installing podman

**Arch Linux**

```bash
sudo pacman -S podman
```

**Fedora, RHEL, Rocky, Alma**: Fedora Workstation already has it. If not:

```bash
sudo dnf install podman
```

**Debian and Ubuntu**

```bash
sudo apt install podman uidmap
```

Check that it works as your own user, without `sudo`:

```bash
podman run --rm docker.io/library/hello-world
```

### Or docker

If you already use docker, it works too. The build runs `docker` as your own
user, so **your user must be able to run `docker` without `sudo`**, usually by
being in the `docker` group, and the docker service must be running. Check with:

```bash
docker run --rm hello-world
```

## Building

The same steps as the bare build, with `-steamrt` added:

```bash
make gs-steamrt-game-cook-pack
```

The first run downloads the SDK (about 3.9 GB) and then builds everything from
scratch, so it takes a while. After that it's an ordinary incremental build: the
build folder and the compiler cache stay on your disk between runs.

When it finishes, `out/build/gcc-ship-steamrt/apps/game/` contains:

```text
Assisi-Game
assets.pak
```

Copy those two files into a folder, and that folder is your game, the same as
in [Packaging your game](packaging.md).

`gs-steamrt` on its own builds without cooking or packing, and the other step
combinations work too, like `make gs-steamrt-cook-pack`.

## Checking the result

```bash
make gs-steamrt-test
```

This builds, runs the game's tests inside the SDK, and then starts the game in
a **bare Debian 11** container that has nothing installed but the game and a
test package. Among the tests is one that fails if the game asks for anything
newer than glibc 2.31. The Debian check downloads about 30 MB the first time.

It runs the game headless, so it proves the game *starts* on an old system. It
doesn't open a window or use a GPU. Before a release, try the game on a real
older system or a virtual machine as well.

## Cleaning up

| Command | What it removes |
|---|---|
| `make clean-gcc-ship-steamrt` | The build folder. The next build starts over but reuses the SDK. |
| `make steamrt-remove` | The SDK, the image built on top of it, the Debian image, and the compiler cache. The next build downloads everything again. |

`make steamrt-fetch` does only the download and image step, without building.
Run it before going offline, or to get the big download out of the way.

## Which distributions can play it

The Steam Runtime build runs on any 64-bit x86 Linux distribution with glibc
2.31 or newer: Ubuntu 20.04+, Debian 11+, Fedora 32+, Mint 20+, Pop!_OS,
openSUSE Leap 15.3+ and Tumbleweed, Arch, RHEL, Rocky and Alma 9+, SteamOS 3,
Bazzite, Silverblue and so on. The player also needs an AVX2 CPU and a Vulkan
driver, the same as for any build.

A few systems can't run prebuilt Linux games at all, from this engine or
anyone else:

- **Distributions without glibc**, such as Alpine and Void's musl edition.
- **NixOS**, unless the player uses `steam-run` or `nix-ld`, as for any
  prebuilt game.
- **Anything older than 2020**, such as Ubuntu 18.04, Debian 10 or RHEL 8.

<details>
<summary>What happens inside</summary>

`make gs-steamrt-...` runs `scripts/steamrt.py`, which:

1. Finds podman, or docker if podman isn't installed.
2. Downloads the SDK image if it isn't already present. The image is pinned to
   an exact version, so a build made today and one made next year use the same
   compiler and libraries.
3. Builds a small image on top of it from `scripts/steamrt/Containerfile`,
   adding a newer CMake. It's rebuilt only when that file or the pinned SDK
   changes.
4. Runs the same `make` target inside the container, as your own user, with
   the repository mounted at the same path. Everything it writes is yours, not
   root's.

Inside the container the build is the `gcc-ship-steamrt` preset: `gcc-ship`
built with the SDK's GCC 14, which only exists in there. Its compiler cache is
kept in `out/steamrt-home/`.

</details>

<details>
<summary>If something goes wrong</summary>

- **"needs podman or docker, and neither is installed"**: install one of them,
  as above.
- **podman fails with an error about `newuidmap` or `/etc/subuid`**: rootless
  podman needs subordinate user ids. Install `uidmap` (Debian and Ubuntu) or
  `shadow-utils` (Fedora), then run
  `sudo usermod --add-subuids 100000-165535 --add-subgids 100000-165535 $USER`
  and log in again.
- **docker fails with "permission denied" on its socket**: your user isn't in
  the `docker` group, or you haven't logged in again since joining it.

</details>
