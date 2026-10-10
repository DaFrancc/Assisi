# Packaging your game

So far you've been running your game inside the editor. Players get
**`Assisi-Game`** instead: the same game with no editor, reading its content
from a single package file.

## The short version

Build the game at the **ship** level, cook the assets, and pack them. One
command does all three.

**Linux**

```bash
./assisi package ship
```

**Windows**

```bash
assisi package ship
```

When it finishes, `out/build/gcc-ship/apps/game/` (or `msvc-ship` on Windows)
contains the two files a player needs:

```text
Assisi-Game          (Assisi-Game.exe on Windows)
assets.pak
```

**Copy those two files into a folder, and that folder is your game.** Zip it up
and send it to someone.

## What those steps do

`package` runs three steps, in order, and stops at the first one that fails:

| Step | What it does | On its own |
|---|---|---|
| **game** | Builds `Assisi-Game`. It isn't part of the normal build, since you use the editor day to day. | `./assisi build ship --game` |
| **cook** | Converts everything in `assets/` into the form the game loads fastest, in `out/build/<build>/cooked/`. Only changed assets are re-cooked. | `./assisi cook ship` |
| **pack** | Bundles the cooked assets into one compressed file, `assets.pak`, next to the game. | `./assisi pack ship` |

For example, after changing only assets, `./assisi cook ship` and then
`./assisi pack ship` update the package without touching the game.

Use a **ship** build for anything players receive. Its package uses the
smallest compression, and the game is fully optimized.

## Before you package

- **Set the startup level** in `assets/config/game.json` (`startupScene`). The
  game opens it straight away; it has no level picker or menu of its own.
- **Open the editor once after adding assets.** Every asset needs its `.aast`
  file for the cook to accept it, and the editor creates them at startup.
- **Keep non-game files out.** The cook refuses any file it doesn't know how to
  handle. To leave files out, list them in `assets/.assisiignore`, one pattern
  per line, like `.gitignore`. It already skips `.blend` and `.zip` files.

## Trying the packaged game

```bash
./assisi package ship
./assisi run game ship
```

`run game` starts the packaged game from its build folder, the way a player
would start it. If the game can't find `assets.pak` next to it, it refuses to
start and says so.

## Releasing, and keeping updates small

When you give players a version, package it with `release` instead, and name
the version:

```bash
./assisi release ship --version 1.0
```

This packages the game exactly like `package`, then keeps a copy of the game
and its `assets.pak` in `releases/1.0/`. Every later `package` and `pack` of
the same level lays its pak out against the newest release: every asset that
didn't change stays where players already have it. Stores like Steam only
download the parts of a file that changed, so an update that touches three
textures is a download of three textures, not the whole game.

- The tool says which release it matched against each time.
- `--fresh` packs from scratch, ignoring every release.
- `--previous <path to a pak>` matches a pak you name instead, for one kept
  somewhere else.
- A version is never overwritten; release 1.1 is a new folder.
- `releases/` is never committed to git (the files are large), and cleaning
  builds never touches it. **Back it up** with your other release files: if it
  is lost, the next update is laid out from scratch and players download more.

## Releasing on Linux

A game built with `./assisi package ship` only runs on Linux systems at least as
new as yours. For a public release, build it with `./assisi package ship
--steam-runtime` instead, which runs on any distribution from about 2020 on and on Steam Deck.
It needs podman or docker. [Building for every Linux
distribution](steam-runtime.md) explains the difference and how to set it up.

<details>
<summary>What a player's computer needs</summary>

- **An x86-64 CPU with AVX2** and **a GPU with a Vulkan driver**, the same as
  for development.
- **Linux:** nothing else. The C++ runtime is built into the game. The one
  exception is the system C library (glibc): it must be at least as new as the
  one the game was built against. A game from `./assisi package ship` needs
  your own distribution's; one from `./assisi package ship --steam-runtime`
  needs glibc 2.31, which every distribution from 2020 on has.
- **Windows:** players may need the Microsoft Visual C++ Redistributable
  installed.
- **A writable game folder.** The game saves `options.json`, logs and crash
  reports next to itself.

</details>

<details>
<summary>Command-line options for the game</summary>

Players don't need any. For testing:

| Option | What it does |
|---|---|
| `--headless` | Run with no window, only the simulation. |
| `--ticks <n>` | Stop after `n` fixed steps. |
| `--verbosity <level>` | Log less: `info`, `warn`, `error`… |
| `--pak <path>` | Read a different package. **Only in debug and dev builds**; a ship build always reads the package beside it. The `ASSISI_PAK` environment variable does the same. |

</details>

<details>
<summary>Running the tools by hand</summary>

The cook and pack steps run two command-line tools, which you can also call
directly:

```bash
out/build/gcc-ship/apps/cook/assisi-cook --source assets --out out/build/gcc-ship/cooked --texture-tier best
out/build/gcc-ship/apps/pack/assisi-pack --cooked out/build/gcc-ship/cooked \
    --out out/build/gcc-ship/apps/game/assets.pak --compress zstd
```

`assisi-pack --previous <last-release.pak>` keeps unchanged assets where the
last release had them, so a player's update download stays small.

</details>

> **Still being built:** there's no one-step "export" or installer yet, and no
> tool that assembles the release folder for you. Copying the two files is the
> current way to ship.
