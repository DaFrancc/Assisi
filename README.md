# Assisi Engine

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
- Make
- CMake 3.28+
- Ninja

The following compilers have been tested:
- Windows — MSVC (Visual Studio 2022+)
- Linux — GCC, Clang

All dependencies are fetched automatically by CMake on first configure — no separate package manager
required: GLFW (windowing/input), GLM (math), [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) (Vulkan
render hardware interface), glslang (build-time GLSL→SPIR-V shader compiler), Dear ImGui + ImPlot
(debug UI), Jolt Physics, stb (image loading), nlohmann/json, Assimp (fetched for planned mesh import),
and doctest (unit tests). Rendering is **Vulkan**: a Vulkan-capable GPU and driver are required at
runtime, but no Vulkan SDK is needed to build (the engine loads Vulkan dynamically).

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

# Or use cmake directly
cmake --preset msvc-debug
cmake --build --preset msvc-debug
```

```bash
# Run the sandbox — the reference app: a level viewer/editor with a fly camera,
# reflected-component inspector, asset browser, and .alvl level save/load.
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
`reflectgen` build tool (`tools/reflectgen`) turns into (de)serialization and inspector code.

### Math
The Math module provides a collection of mathematical functions and data structures commonly used in game development, such as vectors, matrices, and quaternions.
Currently it is mostly a wrapper around the GLM library, but in the future it will include more custom math utilities and optimizations specific to the engine's needs.

### Window
The Window module is responsible for creating and managing the application window, handling input events, and interfacing with the underlying operating system's windowing system.
It manages the GLFW lifecycle via a `GlfwLibrary` shared pointer, exposes a `WindowContext` for the OS window, and an `InputContext` for polling keyboard and mouse state.
It also provides `ActionMap` — a named input action system that maps string action names to key/mouse button bindings and can be loaded from `game.json`.

### Render
The Render module renders the scene through **Vulkan**, via NVIDIA's
[NVRHI](https://github.com/NVIDIA-RTX/NVRHI) hardware-abstraction layer (Windows and Linux; no Vulkan SDK
required to build — only a Vulkan-capable driver at runtime). It provides `RenderSystem` / `VulkanContext`
(device, swapchain, frames-in-flight), `ShaderModule` (loads SPIR-V that glslang compiles from GLSL at
build time), `MeshBuffer` / `Buffer` / `Texture`, and `DefaultResources` (built-in primitive meshes and a
white fallback texture). The default renderer is a **clustered forward** pipeline: `ClusterGrid` bins
point/spot/directional lights into a froxel grid with compute shaders, `MeshPass` shades with a
Cook–Torrance PBR model reading only the lights that touch each cluster, and `PostProcess` supplies
optional MSAA and/or FXAA. `AssetCache` resolves asset paths to GPU meshes/textures, deduplicated by path.

### ECS
The ECS (Entity-Component-System) module provides a framework for working with entity IDs, component storage, sparse sets, queries, and scene management.
Entity handles carry a generation counter so stale handles are detected after a slot is reused. It is
designed to be flexible and efficient, allowing you to create complex game objects and systems without worrying about the underlying data structures or performance implications.
Key types: `Scene`, `SceneRegistry`, `Query` (with `Without<>` exclusion filters), `Registry`, `SparseSet`.

### Runtime
The Runtime module provides ready-to-use components and systems common to most games:
`TransformComponent` (with hierarchy via `ParentComponent` and `PropagateTransforms`),
`MeshRendererComponent`, `CameraComponent`, and point/spot/directional light components.
It ships the default render path — `SceneRenderer`, which ties the camera, the clustered `LightingSystem`,
and mesh drawing together — plus `SceneSerializer` (save/load `.alvl` level files, driven by reflection)
and `DestroyTag` / `DestroyMarked` (deferred end-of-frame entity destruction).
These building blocks compose into your own game logic without modification.

### Physics
The Physics module wraps [Jolt Physics](https://github.com/jrouwe/JoltPhysics) to provide rigid body simulation.
It exposes `PhysicsWorld` (manages the simulation, body creation, stepping, and gravity),
`RigidBodyComponent` (holds a live Jolt `BodyID`, not serialized),
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

Configuration is split in two: `AppConfig` (loaded from `assets/game.json`) holds window, clear-color, and
physics-rate settings a game ships with; `OptionsConfig` (persisted to `options.json`) holds user-facing
runtime options — anti-aliasing mode, MSAA sample count, and VSync/FPS-limit — editable in-app through the
**F12** options window.

## Documentation
Per-module overviews are in the section above, and the public API is documented with Doxygen-style
comments in the headers. Design notes, the NVRHI migration log, and the rolling code-review docket live in
[`docs/`](docs/).

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
