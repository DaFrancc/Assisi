# Troubleshooting and FAQ

## Building

**The configure step fails and names a package.**
Install that package and run the configure again. On Fedora, the most common one
is `libstdc++-static`.

**On Windows, `python3` says "Python was not found", but Python is installed.**
That isn't your Python. Windows ships a placeholder called `python3` (and one
called `python`) that only points you to the Microsoft Store, and Python from
python.org installs as `python` and `py`, never `python3`. `./assisi` skips the
placeholder and uses whichever real one it finds. If it says `assisi: needs
Python 3.9 or newer on PATH`, the placeholder is all it found: install Python
from python.org, or `winget install -e --id Python.Python.3.13`, and open a new
terminal. To get rid of the placeholder, turn off **python.exe** and
**python3.exe** under **Settings → Apps → Advanced app settings → App execution
aliases**.

**The build fails with "ccache: not found" (or similar).**
Install ccache. The build expects it. See [Installation](installation.md). If
you just installed it, open a new terminal: an install only changes `PATH` for
programs started afterwards, so terminals, editors and IDEs that were already
open still can't find it. `./assisi doctor` shows what the current terminal can
see.

**A new dependency version isn't picked up.**
Downloaded dependencies are cached in `out/_deps-src`. Run `./assisi clean
--deps`, then `./assisi configure` for each level you use, e.g. `./assisi
configure debug dev ship`.

**The build stops on a warning.**
Warnings are treated as errors, on purpose, so they get fixed rather than piling
up. Fix the warning. A common one is ignoring the result of `scene.Add<T>(...)`:
write `(void)scene.Add<T>(...)` if you don't need the pointer.

**It builds on Linux, but on Windows, or in `./assisi build ship
--steam-runtime`, a file says a standard name like `std::vector` or
`std::format` doesn't exist.**
The file uses something it never includes. Most Linux builds include the
standard library, nlohmann/json, glm and doctest into every file ahead of time
(a *precompiled header*), so a missing `#include <vector>` goes unnoticed there;
MSVC builds and the Steam Runtime build don't. Add the include.

**reflectgen stops the build with an error about my header.**
reflectgen reads the `ASYSTEM`, `ACOMP` and `AFIELD` annotations, and its
message says what's wrong. The usual causes:

- A field of type `int`, `long`, `char` or `std::string`. Use `int32_t`,
  `int64_t`, or the string type for the job. See
  [Your own components](your-own-components.md) and [Strings](strings.md).
- A `Core::PooledString` in a struct without exactly one `Core::StringPool`.
- An `ASYSTEM` without `name = "..."`, or two systems with the same name.
- An `after =` or `before =` naming a system that doesn't exist.
- A system that isn't a plain function returning `void` and taking
  `SystemContext &`.

## My code doesn't seem to run

**My system doesn't run.**

1. Is its name in the level's **Systems** panel? A system only runs in levels
   that list it.
2. Are you playing? Systems don't run while you're editing. Press **F5**.
3. Is it `activeWorldOnly`, and is the world you're looking at a different one?
4. Is the header under `apps/game/src/`, and did you rebuild?

**My system doesn't appear in the Add System box.**
The build didn't find it. Check that the `ASYSTEM` is in a **header** (`.hpp`),
not a `.cpp`, that the header is somewhere under `apps/game/src/`, and that you
rebuilt the editor.

**My component isn't in Add Component.**
Same checks as above, with `ACOMP()` instead of `ASYSTEM`.

**My component's field isn't in the inspector, or isn't saved.**
It's missing `AFIELD()`.

**I change an object's position in code, but it doesn't move.**
You're writing through `Get`. Use `GetMut`, or ask the query for `Mut<T>`. See
[Entities and components](entities-and-components.md). A write through `GetMut`
moves a physics object too, but a dynamic body keeps its velocity and carries on
falling from where you put it; use `Teleport` to stop it as well. See
[What writing a `Transform` does](physics-scene-sync.md#what-writing-a-transform-does).

**My input code crashes on the server, or in tests.**
`ctx.input` is `nullptr` when there's no window. Check it before using it. See
[Input](input.md).

**A key press is sometimes missed.**
You're reading a one-frame "pressed" in a `FixedUpdate` system, which doesn't
run every frame. Read input in `Update`.

## Levels and assets

**A level won't open. The log says it "names a system this build does not
declare".**
The level lists a system that no longer exists, usually because it was renamed or
deleted in code. See [Levels and blueprints](levels-and-blueprints.md) for how to
fix it.

**The editor starts with an empty world.**
Pass a level with `-l levels/YourLevel.alvl`, or load one from the **Levels**
panel.

**Save is greyed out.**
A level that has never been saved needs **Save As** first. Saving is also
disabled while playing.

**My model or texture doesn't show up in the asset browser.**
Press **Reimport** in the asset browser, or restart the editor. Models must be
glTF (`.gltf` or `.glb`), and textures PNG, JPEG, BMP or TGA.

## Running

**The editor or game exits immediately, or complains about Vulkan.**
Check that your GPU driver is installed and supports Vulkan. On Linux,
`vulkaninfo` (from the `vulkan-tools` package) shows whether Vulkan works.

**"Illegal instruction" on startup.**
The CPU doesn't support AVX2. The engine requires it.

**The packaged game says it can't read the content package.**
`assets.pak` must be next to `Assisi-Game`. See
[Packaging your game](packaging.md).

**The packaged game says it can't start because of the startup scene.**
Check `startupScene` in `assets/config/game.json`, and that the level is in the
package: cook and pack again.

## Where to look

- **Logs** are written next to the executable, named like
  `assisi-20260918-132711-44514.log`. The latest one usually explains a failure.
- **Crashes** that make no sense are often memory bugs. Try an `asan` build; see
  [Build types](build-types.md).
- **Still stuck?** Open an issue on the
  [issue tracker](https://github.com/DaFrancc/Assisi/issues).
