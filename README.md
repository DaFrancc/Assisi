# Assisi Engine

<p align="center">
  <img src="atom-frames/atom-spinner-v4-60.webp" alt="Assisi atom spinner" width="120">
</p>

Assisi is a modern C++ game engine for Windows and Linux focused on building performant, modular systems for
real-time games. This project is primarily for my own education and for my own
needs first. Suggestions, PRs, forks, and bug reports are welcome. Use of this
codebase or engine in your own projects or forks is 100% welcome and free of charge if in compliance
with the license. I kindly ask that you credit the engine and myself
(Francisco Vivas Puerto aka "DaFrancc") in both the repo and any games you make with this engine. Credit is not required, but it would be greatly appreciated.

## Getting Started
### 1. Clone the repo.
```bash
git clone https://github.com/DaFrancc/Assisi.git
```

### 2. Install Tools
- [Make](https://www.gnu.org/software/make/)
- [CMake](https://cmake.org/) 3.28+
- [Ninja](https://ninja-build.org/)

The following compilers have been tested:
- Windows — [MSVC](https://visualstudio.microsoft.com/) (Visual Studio 2022+)
- Linux — [GCC](https://gcc.gnu.org/), [Clang](https://clang.llvm.org/)

**Everything below is fetched and built automatically by CMake on first configure** — there is no
separate package manager step, and nothing to install by hand:

| Dependency | What it does |
|---|---|
| [GLFW](https://github.com/glfw/glfw) | Window creation and input |
| [GLM](https://github.com/g-truc/glm) | Vector/matrix/quaternion math |
| [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) | Render hardware interface over Vulkan |
| [glslang](https://github.com/KhronosGroup/glslang) | Compiles GLSL → SPIR-V at build time |
| [Dear ImGui](https://github.com/ocornut/imgui) | Immediate-mode UI for the editor and debug panels |
| [ImPlot](https://github.com/epezent/implot) | Plots and graphs for those panels |
| [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) | The editor's move/rotate/scale gizmo |
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | Rigid-body simulation |
| [fastgltf](https://github.com/spnda/fastgltf) | glTF mesh/material import |
| [stb](https://github.com/nothings/stb) | Image decoding (PNG, JPEG, …) |
| [libwebp](https://github.com/webmproject/libwebp) | WebP decoding, including animated |
| [FreeType](https://github.com/freetype/freetype) | Font rasterization |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON for configs and level files |
| [doctest](https://github.com/doctest/doctest) | Unit-test framework |
| [Assimp](https://github.com/assimp/assimp) | Multi-format mesh import — **off by default** |

[Assimp](https://github.com/assimp/assimp) is the deferred catch-all import backend (FBX/OBJ/DAE/…) and
stays off (`ASSISI_ENABLE_ASSIMP`) so its heavy build doesn't tax every configure; fastgltf covers the
runtime glTF path today. Rendering is **Vulkan**: a Vulkan-capable GPU and driver are required at
runtime, but no Vulkan SDK is needed to build (the engine loads Vulkan dynamically).

**Minimum CPU:** x86-64 with **AVX2** (Intel Haswell 2013+ / AMD Zen 2015+, and the FMA/F16C/LZCNT/BMI
extensions that ship alongside it). This is a deliberate baseline — the engine is compiled with these
instruction sets enabled globally for SIMD performance, so binaries will crash with an
illegal-instruction fault on older CPUs.

### 3. Configure
#### Windows (from a Visual Studio Developer Command Prompt):
```bash
make configure-msvc
```
#### Linux:
```bash
make configure-gcc    # GCC
make configure-clang  # Clang
```
- Note: The first configure will download and build all dependencies. This takes several minutes. Subsequent runs are fast.
#### MacOS:
- Currently unsupported, but you are free to fiddle around with it and submit a pull request.

### 3b. (Optional) Create a New App
The setup scripts scaffold a new app under `apps/` and generate a `CMakeUserPresets.json` for it:
#### Windows:
```bash
powershell -ExecutionPolicy Bypass -File .\setup.ps1
```
#### Linux:
```bash
chmod +x ./setup.sh && ./setup.sh
```

### 4. Configure & Build

#### Which build should I use?

There are five build *variants*. They differ in how much the compiler optimizes, how much debug
information is kept, and whether extra runtime checking is compiled in.

| Variant | Speed | Use it when |
|---|---|---|
| **debug** | slowest (10–40×) | Stepping through code in a debugger. Asserts on, nothing optimized. |
| **dev** | fast | **The everyday build.** Optimized, but keeps enough symbols for a usable stack trace. |
| **ship** | fastest | Releasing, and **any performance measurement**. |
| **asan** | ~2–3× slower | Chasing a crash, a memory leak, or corrupted-looking data. |
| **tsan** | ~5–15× slower | Chasing a race — random-looking bugs in threaded code. |

Rules of thumb: **build `dev` day to day**, drop to `debug` only when you need a debugger, and
**never judge performance from a `debug` build** — it does not merely make everything uniformly
slower, it changes *which* code looks slow. Reach for `asan`/`tsan` when something is wrong rather
than routinely; they are diagnostic tools, not a build you live in.

<details>
<summary><b>More detail on each</b></summary>

**debug** — `CMAKE_BUILD_TYPE=Debug`. No optimization at all: every small function is a real function
call, and templated math (GLM, the ECS) suffers worst. This is what makes it 10–40× slower than
`ship`, and why it is misleading to profile: a measured example from this engine had one function take
0.641 ms in `debug` and 0.017 ms in `ship` — a 37× difference concentrated in one place, which
reorders the whole profile. Its value is that the debugger shows you every variable and never
optimizes a line away.

**dev** — `CMAKE_BUILD_TYPE=RelWithDebInfo`. Optimized like a release build but keeps debug info, so
it runs at roughly shipping speed while a crash still gives a readable stack trace. This is the
sensible default for ordinary work: fast enough that the editor feels right, debuggable enough that
you can find out why something broke. Inlining means the debugger will sometimes skip or reorder
lines.

**ship** — `CMAKE_BUILD_TYPE=Release`. Full optimization, no debug info, and fast-math enabled
(`ASSISI_ENABLE_FAST_MATH`, on by default). This is what players run, and therefore the only build
whose performance numbers mean anything. A crash here gives you little to work with, so reproduce in
`dev` before investigating.

**asan** — `debug` plus **AddressSanitizer and UndefinedBehaviorSanitizer**. Instruments every memory
access and turns silent corruption into an immediate, precise report: use-after-free, buffer overrun,
leaks at exit, signed overflow, bad casts. If a bug shows up as "it crashes somewhere unrelated" or
"this value is nonsense", run it here first — it usually names the exact line. Costs ~2–3× runtime and
a lot of memory.

**tsan** — `debug` plus **ThreadSanitizer**, which finds *data races*: two threads touching the same
memory with no synchronization. It reports races even when the run looked fine, which matters because
races usually do look fine right up until they don't. Relevant to the job system, async asset loading,
and physics. Very slow, and **mutually exclusive with asan** — the two cannot be combined, so run them
separately.

**`-chiara` variants** — orthogonal to all of the above. They add the profiler (`ASSISI_ENABLE_CHIARA`)
and GPU debug markers rather than changing optimization, so `gcc-ship-chiara` is a full shipping build
that can also record a capture. That combination is the one to profile with.

On Linux, `scripts/run-sanitized.sh` launches the sandbox under a sanitizer build and captures the
report to a log file, so a diagnostic survives even if the window dies.

</details>

#### Building

After initialization, you can build individual presets or use the all-in-one Makefile targets:

```bash
# Configure + build all presets for a toolchain
make msvc    # Windows
make gcc     # Linux (GCC)
make clang   # Linux (Clang)

# Or build a specific preset
make msvc-debug  # (alias: md)
make msvc-dev    # (alias: mv)
make msvc-ship   # (alias: ms)
make gcc-debug   # (alias: gd)
make gcc-dev     # (alias: gv)
make gcc-ship    # (alias: gs)
make clang-debug # (alias: cd)
make clang-dev   # (alias: cv)
make clang-ship  # (alias: cs)

# Sanitizer builds (no short aliases). ASan is available on MSVC too; TSan is
# Linux-only, and the two can never be combined in one build.
make gcc-asan    # AddressSanitizer + UBSan   (also: msvc-asan, clang-asan)
make gcc-tsan    # ThreadSanitizer            (also: clang-tsan)

# Every preset has a `-chiara` variant that compiles the profiler in (see the
# Chiara module below). Suffix the alias with `-c`:
make gcc-ship-chiara  # (alias: gs-c) — the build worth profiling
make gcc-debug-chiara # (alias: gd-c)

# Or use cmake directly
cmake --preset msvc-debug
cmake --build --preset msvc-debug
```

```bash
# Run the sandbox — the reference app. It is a thin consumer of the Editor
# library (a few hundred lines: argument parsing plus two demo systems); the
# editor itself lives in modules/Editor.
./out/build/msvc-debug/apps/sandbox/Assisi-Sandbox.exe
```

### 5. Run the Tests (optional)
The unit tests (doctest) and the `reflectgen` golden-file tests run through CTest presets that
mirror the build presets:

```bash
ctest --preset msvc-debug   # build first, then run all suites
ctest --preset msvc-debug -R ECS   # a single suite
```

## Understanding Assisi's Module Layout
Assisi is organized into several modules, each responsible for a specific aspect of the engine. All modules compile as static libraries under the `Assisi::` CMake namespace. Below is an overview of each module and its responsibilities:

### Core
The Core module contains the fundamental utilities and infrastructure shared across all other modules:
the `Logger` (console and file sinks), `AssetSystem` (a virtual filesystem with a read-only asset root
and a writable per-user root, returning `std::expected`), error types, `Prelude.hpp` (common includes),
and the `EventQueue` (a per-frame typed event bus for decoupled inter-system communication). It also
houses the **reflection** system — `ACOMP()`/`AFIELD()` annotations and a `ComponentRegistry` that the
`reflectgen` build tool (`tools/reflectgen`) turns into (de)serialization and inspector code — and
`JobSystem`, the engine's task pool (worker threads plus a main-thread queue for work that has to land
at a safe point in the frame).

### Math
The Math module provides a collection of mathematical functions and data structures commonly used in game development, such as vectors, matrices, and quaternions.
Currently it is mostly a wrapper around the GLM library, but in the future it will include more custom math utilities and optimizations specific to the engine's needs.

### Window
The Window module is responsible for creating and managing the application window, handling input events, and interfacing with the underlying operating system's windowing system.
It manages the GLFW lifecycle via a `GlfwLibrary` shared pointer, exposes a `WindowContext` for the OS window, and an `InputContext` for polling keyboard and mouse state.
It also provides `ActionMap` — a named input action system that maps string action names to key/mouse button bindings and can be loaded from `game.json`.

### Geometry
The Geometry module owns mesh and material *data*, independent of the GPU — so importers and tools can
use it without a Vulkan device. It provides `MeshData` / `Vertex` / `SubMesh` / `LodRange`, `Bounds`
(bounding spheres and AABBs used by the frustum cull), `DefaultMeshes` (the built-in cube/sphere/plane
primitives), and `MaterialData` / `MaterialFile` (the `.amat` on-disk material). `MeshImporter` and
`AssetImport` load glTF through fastgltf, exploding a file's materials into individual assets on import.

### Render
The Render module renders the scene through **Vulkan**, via NVIDIA's
[NVRHI](https://github.com/NVIDIA-RTX/NVRHI) hardware-abstraction layer (Windows and Linux; no Vulkan SDK
required to build — only a Vulkan-capable driver at runtime). It provides `RenderSystem` / `VulkanContext`
(device, swapchain, frames-in-flight), `ShaderModule` (loads SPIR-V that glslang compiles from GLSL at
build time), and `MeshBuffer` / `Buffer` / `Texture`. The default renderer is a **clustered forward**
pipeline: `ClusterGrid` bins point/spot/directional lights into a froxel grid with compute shaders,
`MeshPass` shades with a Cook–Torrance PBR model reading only the lights that touch each cluster, and
`PostProcess` supplies optional MSAA and/or FXAA. Drawing is **GPU-driven**: `MeshCuller` frustum-culls
on the GPU and builds indirect commands, so a whole scene issues a single `drawIndexedIndirect`, and
`GeometryArena` keeps mesh data in shared vertex/index buffers. `AssetCache` resolves asset paths to GPU
meshes/textures (deduplicated by path, including the built-in `prim://` primitives and fallback
textures) and streams uploads off the main thread. `OutlinePass` / `IconPass` / `LinePass` are the
editor's overlay passes — opt-in, so a game never builds them. `GpuMarker.hpp` labels command buffers
for RenderDoc and Nsight; see [`docs/gpu-profiling-guide.md`](docs/gpu-profiling-guide.md).

### ECS
The ECS (Entity-Component-System) module provides a framework for working with entity IDs, component storage, sparse sets, queries, and scene management.
Entity handles carry a generation counter so stale handles are detected after a slot is reused. It is
designed to be flexible and efficient, allowing you to create complex game objects and systems without worrying about the underlying data structures or performance implications.
Key types: `Scene`, `SceneRegistry`, `Query` (with `Without<>` exclusion filters), `Registry`, `SparseSet`.

### Runtime
The Runtime module provides ready-to-use components and systems common to most games:
`Transform` (with hierarchy via `Parent` and `PropagateTransforms`),
`MeshRenderer`, `Camera`, and point/spot/directional light components.
It ships the default render path — `SceneRenderer`, which ties the camera, the clustered `LightingSystem`,
and mesh drawing together — plus `SceneSerializer` (save/load `.alvl` level files, driven by reflection),
`AssetResolve` (GUID/path asset references on components) and `Lifecycle`'s `DestroyTag` /
`DestroyMarked` (deferred end-of-frame entity destruction).
These building blocks compose into your own game logic without modification.

### Physics
The Physics module wraps [Jolt Physics](https://github.com/jrouwe/JoltPhysics) to provide rigid body simulation.
It exposes `PhysicsWorld` (manages the simulation, body creation, stepping, and gravity),
`RigidBody` (holds a live Jolt `BodyID`, not serialized),
and `RigidBodyDescriptor` (a serializable description of a body's shape and static/dynamic flags).
Call `PhysicsWorld::Clear()` before loading a new level to destroy all tracked bodies.

### Debug
The Debug module wraps [Dear ImGui](https://github.com/ocornut/imgui) and
[ImPlot](https://github.com/epezent/implot) with a GLFW + **Vulkan** backend.
`DebugUI::Initialize(window, vulkanContext)` must be called after the Vulkan context exists.
Override `OnImGui()` in your application class to draw debug panels and overlays.

### App
The App module provides the application framework on top of the lower-level modules.
`Application` is the base class: it runs a fixed-timestep physics loop (default 60 Hz, `AppConfig::physicsHz`)
and paces rendering either to the display refresh (VSync) or to an optional FPS cap, selectable at runtime.
Override `OnStart`, `OnFixedUpdate(dt)`, `OnUpdate(dt)`, and `OnRender(RenderFrame&)`, plus optionally
`OnImGui()`, `OnShutdown()`, `OnResize()`, and `OnRenderTargetsChanged()`.

`SystemRegistry` provides dependency-ordered, phase-based system registration (`PreUpdate`, `FixedUpdate`,
`Update`, `PostUpdate`) via `After()` / `Before()` constraints; render systems register separately through
`RegisterRender`. Named input actions come from an `ActionMap` loaded from `assets/game.json`.

`World` and `WorldManager` hold **several scenes resident at once** — each world owns its scene, its
physics world and its own instance of the registered systems, and carries a `WorldState`
(`Loading` / `Active` / `Dormant`). Systems are bound per world through named **profiles**, so a level
selects which systems it runs rather than the choice being global. `Application` also owns a
`Core::JobSystem` (task pool, `Jobs().Run()` / `RunOnMain()`), drained at a fixed point each frame —
that is where background asset loads publish.

Configuration is split in two: `AppConfig` (loaded from `assets/game.json`) holds window, clear-color, and
physics-rate settings a game ships with; `OptionsConfig` (persisted to `options.json`) holds user-facing
runtime options — anti-aliasing mode, MSAA sample count, and VSync/FPS-limit — editable in-app through the
**F11** options window.

### Editor
The Editor module is the level editor, built **as a library** rather than baked into an executable, so a
game and its editor are two thin targets over the same code. `EditorApp` derives from `App::Application`
and adds the fly camera, entity list, reflected-component inspector, transform gizmo, asset browser,
collider wireframes, play/pause/stop, and `.alvl` level load/save. `EditHistory` is the undo/redo system
(Ctrl-Z), which captures at every edit site and survives entering and leaving play mode. `apps/sandbox`
is a few hundred lines on top of it. Editor overlays are opt-in at the renderer level
(`enableEditorVisuals`), so a game build never creates those pipelines or loads editor assets.

### Chiara
Chiara is the performance and memory analyzer — an always-on-when-compiled-in frame profiler, named
scopes and counters recorded into lock-free per-thread rings and exported as Chrome Trace JSON that
opens in [Perfetto](https://ui.perfetto.dev). `ASSISI_PROFILE_SCOPE` / `ASSISI_PROFILE_COUNTER` cost
~18 ns per scope; in a build without `ASSISI_ENABLE_CHIARA` every macro compiles to nothing at all, so
instrumentation cannot bit-rot. Press **F9** in the sandbox for the capture panel — snapshot the recent
past out of the ring, or stream a longer session to disk.

It has no dependencies, which is what lets `Core` (and therefore everything) sit above it. The design
notes are in [`docs/chiara-design-notes.md`](docs/chiara-design-notes.md), and
[`scripts/chiara-analyze.py`](scripts/chiara-analyze.py) reads a capture from the terminal when you want
numbers rather than a flame chart.

## Documentation
Per-module overviews are in the section above, and the public API is documented with Doxygen-style
comments in the headers. Design notes and the rolling code-review docket live in [`docs/`](docs/) —
including the architecture notes for [clustered lighting](docs/light-culling-design-notes.md),
[GPU-driven rendering](docs/gpu-driven-rendering-design-notes.md),
[multi-scene worlds](docs/multi-scene-design-notes.md), the [job system](docs/job-system-design-notes.md),
[asset streaming](docs/asset-streaming-design-notes.md) and [Chiara](docs/chiara-design-notes.md).

Two practical guides:
- [`docs/gpu-profiling-guide.md`](docs/gpu-profiling-guide.md) — capturing and reading a frame with
  RenderDoc, written for someone who has never used a GPU profiler. Includes the Wayland/NVIDIA
  pitfalls, which are not guessable.
- [`docs/remaining-work.md`](docs/remaining-work.md) — the running list of known gaps.

## Useful Links
- [Issue Tracker](https://github.com/DaFrancc/Assisi/issues)

## AI Notice
This project uses AI to help develop this project for the main purpose of education alongside some code generation, documentation,
bug spotting, bug fixing, and temporary art creation (i.e. placeholders for the sake of development, but never to end
up in full releases).

You are free to commit code that was written with the use of AI, and it will be reviewed to the same standard as code
that was not written with AI. You do not need to disclose exactly which ideas, lines, or commits were aided with the
use of AI, but you are free to do so. The only exception is that you must clearly disclose any art assets that
were created with AI such as: textures, 3D models, photos, videos, audio, and any other form of creative media that
goes into this engine.

This notice only applies to this repo. Any forks of this repo or software made using this engine do not need to follow
these guidelines regarding the use of AI.
