# Build types

The engine can be built in several ways. They differ in how much the compiler
optimizes and how much debugging help is kept.

**The short version:** use **dev** every day, **debug** when you need to step
through code in a debugger, and **ship** for anything you give to players or any
time you measure performance.

| Build | Speed | Use it for |
|---|---|---|
| **dev** | fast | Everyday work. Optimized, but crashes still give a readable stack trace. |
| **debug** | very slow (10–40×) | Stepping through code line by line in a debugger. |
| **ship** | fastest | Releases, and measuring performance. |

## Building each one

Every build has a long name and a short alias. The first letter is the compiler
(`g`=GCC, `c`=Clang, `m`=MSVC) and the second is the build (`d`=debug, `v`=dev,
`s`=ship):

| | GCC | Clang | MSVC |
|---|---|---|---|
| **debug** | `make gd` | `make cd` | `make md` |
| **dev** | `make gv` | `make cv` | `make mv` |
| **ship** | `make gs` | `make cs` | `make ms` |

Each build lives in its own folder, like `out/build/gcc-ship/`, so switching
between them doesn't throw away the others.

On Linux, ship has a second form for public releases, `make gs-steamrt`, which
builds inside Valve's Steam Runtime so the game runs on other people's
distributions too. [Building for every Linux distribution](steam-runtime.md)
explains it.

## Never judge performance in debug

A debug build isn't just a uniformly slower build. It changes *which* code is
slow, because small functions that the optimizer normally erases become real
function calls. Something that looks like your biggest cost in debug can be
nothing in ship. Measure performance in a **ship** build.

<details>
<summary>Sanitizer builds, for hunting hard bugs</summary>

Two extra builds exist for tracking down bugs that are hard to find otherwise.
They're slow, and you only use them when something is wrong.

- **asan** (`make gcc-asan`, `make clang-asan`, `make msvc-asan`) catches memory
  bugs: using memory after freeing it, writing past the end of an array, leaks.
  If your game crashes somewhere that makes no sense, or a value is garbage,
  try this first. It usually points at the exact line.
- **tsan** (`make gcc-tsan`, `make clang-tsan`, Linux only) catches data races:
  two threads touching the same memory without synchronization.

The two can't be combined in one build.

On Linux, `scripts/run-sanitized.sh` runs the editor under a sanitizer build and
saves the report to a log file.

</details>

<details>
<summary>Profiler builds</summary>

Adding `-chiara` to any build compiles in the engine's profiler, Chiara. For
example, `make gcc-ship-chiara` (alias `make gs-c`) is a ship build you can
profile. Press **F9** in the editor to open the capture panel. Captures open in
[Perfetto](https://ui.perfetto.dev).

</details>

<details>
<summary>What each build means in CMake terms</summary>

| Build | `CMAKE_BUILD_TYPE` | Notes |
|---|---|---|
| debug | `Debug` | No optimization, asserts on. |
| dev | `RelWithDebInfo` | Optimized, with debug info. |
| ship | `Release` | Full optimization and fast-math, no debug info. |
| asan | `Debug` | Plus AddressSanitizer and UndefinedBehaviorSanitizer. |
| tsan | `Debug` | Plus ThreadSanitizer. |

The presets are defined in `CMakePresets.json`, and you can also use CMake
directly: `cmake --build --preset gcc-dev`.

</details>
