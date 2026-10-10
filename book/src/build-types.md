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

Name the level after any [build tool](build-tool.md) command:

```bash
./assisi build debug
./assisi build dev
./assisi build ship
```

The level and the compiler are separate choices. Without `--compiler` you get
your usual one, GCC on Linux and MSVC on Windows. To use another, add it:

```bash
./assisi build ship --compiler clang
```

To make Clang your usual compiler, run `./assisi default --compiler clang`.
Several levels at once work too: `./assisi build debug dev ship`.

Each combination lives in its own folder, like `out/build/gcc-ship/` or
`out/build/clang-debug/`, so switching between them doesn't throw away the
others.

On Linux, ship has a second form for public releases, `--steam-runtime`, which
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

- **The address sanitizer** (`--sanitize address`, any compiler) catches memory
  bugs: using memory after freeing it, writing past the end of an array, leaks.
  If your game crashes somewhere that makes no sense, or a value is garbage,
  try this first. It usually points at the exact line.
- **The thread sanitizer** (`--sanitize thread`, GCC and Clang only) catches
  data races: two threads touching the same memory without synchronization.

Both are debug builds. Start the editor under one with `./assisi run editor
debug --sanitize address`, or run the tests under it with `./assisi test debug
--sanitize address`.

The two can't be combined in one build.

On Linux, `scripts/run-sanitized.sh` runs the editor under a sanitizer build and
saves the report to a log file.

</details>

<details>
<summary>Profiler builds</summary>

Adding `--profiler` to any build compiles in the engine's profiler, Chiara. For
example, `./assisi run editor ship --profiler` builds and starts a ship build
you can profile. Press **F9** in the editor to open the capture panel. Captures
open in [Perfetto](https://ui.perfetto.dev).

</details>

<details>
<summary>What each build means in CMake terms</summary>

| Build | `CMAKE_BUILD_TYPE` | Notes |
|---|---|---|
| debug | `Debug` | No optimization, asserts on. |
| dev | `RelWithDebInfo` | Optimized, with debug info. |
| ship | `Release` | Full optimization and fast-math, no debug info. |
| debug `--sanitize address` | `Debug` | Plus AddressSanitizer and UndefinedBehaviorSanitizer. |
| debug `--sanitize thread` | `Debug` | Plus ThreadSanitizer. |

Underneath, each combination is a CMake *preset* in `CMakePresets.json`, named
compiler then level: `gcc-dev`, `clang-ship`, `gcc-ship-chiara` with the
profiler, `gcc-asan` for an address sanitizer build. You can use CMake directly
once a build has been configured: `cmake --build --preset gcc-dev`.

</details>
