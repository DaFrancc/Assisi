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

## When the game crashes

When the game or the editor crashes on Linux, a window opens saying it stopped,
why, and where. The same information is saved in files next to the executable,
named for the run, like its log:

| File | What it holds |
|---|---|
| `crash-20260918-132711-44514.txt` | The **crash log**: why the game stopped, then **Stack**, the functions that were running when it did, the one that stopped first. |
| `crash-20260918-132711-44514.dmp` | The **minidump**: every thread's stack, and the memory around each one, for a debugger. |
| `assisi-20260918-132711-44514.log` | The run's log, which ends where the game stopped. |

If a player sends you a crash, these three files are what to ask for. The
window's **Open folder** button opens the folder they are in. Nothing is sent
anywhere by itself.

On Windows, a crash saves only the minidump and opens no window. Open the
minidump in Visual Studio.

### Reading the stack

Each line of **Stack** is a function, with the file and line it was at, in a
dev or debug build:

```text
Stack:
  #0  Game::SpawnWave(int)  at /home/you/MyGame/apps/game/src/Waves.cpp:88  (Assisi-Game + 0x8c1f20)
  #1  Game::WaveSystem(Assisi::App::SystemContext&)  at /home/you/MyGame/apps/game/src/Waves.cpp:41  (Assisi-Game + 0x8c2a14)
```

`#0` is where the game stopped, and each line below it is what called the line
above. A ship build carries no function names, so its lines show only the file
the code is in and where in it, like `Assisi-Game + 0x8c1f20`.

### Opening the minidump

The minidump shows every thread, and lets you look at variables, which helps
when the crash is in one thread but the cause is in another. Open it in
`lldb`, with the exact executable that crashed:

```bash
lldb Assisi-Game -c crash-20260918-132711-44514.dmp
```

Then `bt` shows the crashing thread's stack, and `bt all` shows every thread.

### Stopping the game on purpose

Sometimes your code finds a problem the game cannot carry on past, such as
content that needs something this build doesn't have. `Core::Abort` stops the
game there, in every build, the same way a crash does: the window opens and the
crash log gives your message as the reason.

```cpp
#include <Assisi/Core/Abort.hpp>

if (wave == nullptr)
{
    Assisi::Core::Abort("Wave {} is in the level but not in the wave table.", index);
}
```

Use it rarely. Choose by what kind of problem it is:

| The problem | Write |
|---|---|
| Something can fail and the game can cope, such as a missing optional file | Log it with `Log::Warn` or `Log::Error` and carry on. |
| A bug in your own code if it ever happens | `ASSISI_ASSERT(condition, "why")`. It stops a debug build, and costs nothing in a ship build, where it is left out. |
| The game cannot carry on, in any build | `Core::Abort("why")`. |

## Where to look

- **Logs** are written next to the executable, named like
  `assisi-20260918-132711-44514.log`. The latest one usually explains a failure.
- **Crashes** leave a crash log and a minidump next to the log; see
  [When the game crashes](#when-the-game-crashes). A crash that makes no sense
  is often a memory bug. Try an `asan` build; see [Build types](build-types.md).
- **Still stuck?** Open an issue on the
  [issue tracker](https://github.com/DaFrancc/Assisi/issues).
