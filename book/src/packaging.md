# Packaging your game

So far you've been running your game inside the editor. Players get
**`Assisi-Game`** instead: the same game with no editor, reading its content
from a single package file.

## The short version

Build a **ship** build of the game, cook the assets, and pack them. Run the one
for your system.

**Linux**

```bash
make gsgkp
```

**Windows**

```bash
make msgkp
```

(`gs` is gcc-ship and `ms` is msvc-ship, followed by the steps: game, cook,
pack.)

When it finishes, `out/build/gcc-ship/apps/game/` (or `msvc-ship` on Windows)
contains the two files a player needs:

```text
Assisi-Game          (Assisi-Game.exe on Windows)
assets.pak
```

**Copy those two files into a folder, and that folder is your game.** Zip it up
and send it to someone.

## What those steps do

The letters after `gs` are steps, run in order: **g**ame, coo**k**, **p**ack.

| Step | What it does |
|---|---|
| **game** (`g`) | Builds `Assisi-Game`. It isn't part of the normal build, since you use the editor day to day. |
| **cook** (`k`) | Converts everything in `assets/` into the form the game loads fastest, in `out/build/<build>/cooked/`. Only changed assets are re-cooked. |
| **pack** (`p`) | Bundles the cooked assets into one compressed file, `assets.pak`, next to the game. |

You can run any subset. For example, `make gsk` only cooks, and `make gsp` packs
the last cook again.

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
make gsgkp
./out/build/gcc-ship/apps/game/Assisi-Game
```

If the game can't find `assets.pak` next to it, it refuses to start and says so.

<details>
<summary>What a player's computer needs</summary>

- **An x86-64 CPU with AVX2** and **a GPU with a Vulkan driver**, the same as
  for development.
- **Linux:** nothing else. The C++ runtime is built into the game. The one
  exception is the system C library (glibc): it must be at least as new as the
  one on the machine you built on. Building on an older, stable distribution
  reaches more players.
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

The make steps run two command-line tools, which you can also call directly:

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
