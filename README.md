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

All library dependencies (GLFW, GLM, Assimp, stb, Glad, Jolt Physics, Dear ImGui, nlohmann_json) are fetched automatically by CMake on first configure — no separate package manager required.

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
# Run the sandbox
./out/build/msvc-debug/apps/sandbox/Assisi-Sandbox.exe
```

## Understanding Assisi's Module Layout
Assisi is organized into several modules, each responsible for a specific aspect of the engine. All modules compile as static libraries under the `Assisi::` CMake namespace. Below is an overview of each module and its responsibilities:

### Core
The Core module contains the fundamental utilities and infrastructure shared across all other modules.
It includes the `Logger` (console and file sinks), `AssetSystem` (asset discovery and loading via `std::expected`),
error types, `Prelude.hpp` (common includes), and the `EventQueue` (a per-frame typed event bus for decoupled inter-system communication).

### Math
The Math module provides a collection of mathematical functions and data structures commonly used in game development, such as vectors, matrices, and quaternions.
Currently it is mostly a wrapper around the GLM library, but in the future it will include more custom math utilities and optimizations specific to the engine's needs.

### Window
The Window module is responsible for creating and managing the application window, handling input events, and interfacing with the underlying operating system's windowing system.
It manages the GLFW lifecycle via a `GlfwLibrary` shared pointer, exposes a `WindowContext` for the OS window, and an `InputContext` for polling keyboard and mouse state.
It also provides `ActionMap` — a named input action system that maps string action names to key/mouse button bindings and can be loaded from `game.json`.

### Render
The Render module is responsible for rendering graphics to the screen. It provides an abstraction layer over the underlying graphics API (currently OpenGL,
but in the future it may support other APIs like Vulkan or DirectX). It includes `RenderSystem`, `Shader`, `MeshBuffer`, and `DefaultResources` (built-in primitive meshes).
Hopefully in the future it will be as customizable as Unity's rendering pipeline,
allowing you to create custom render passes, shaders, and materials to achieve the exact look you want for your game.

### ECS
The ECS (Entity-Component-System) module provides a framework for working with entity IDs, component storage, sparse sets, queries, and scene management.
It is designed to be flexible and efficient, allowing you to create complex game objects and systems without worrying about the underlying data structures or performance implications.
Key types: `Scene`, `SceneRegistry`, `Query`, `SparseSet`.

### Runtime
The Runtime module provides ready-to-use components and systems that are common across most games.
It includes `TransformComponent`, `MeshRendererComponent`, `Camera`, `DrawScene` (renders all mesh entities),
`SceneSerializer` (save/load `.alvl` level files), and `DestroyTag` (mark entities for end-of-frame destruction).
These building blocks can be composed into your own game logic without modification.

### Physics
The Physics module wraps [Jolt Physics](https://github.com/jrouwe/JoltPhysics) to provide rigid body simulation.
It exposes `PhysicsWorld` (manages the simulation, body creation, stepping, and gravity),
`RigidBodyComponent` (holds a live Jolt `BodyID`, not serialized),
and `RigidBodyDescriptor` (a serializable description of a body's shape and static/dynamic flags).
Call `PhysicsWorld::Clear()` before loading a new level to destroy all tracked bodies.

### Debug
The Debug module wraps [Dear ImGui](https://github.com/ocornut/imgui) with a GLFW + OpenGL3 backend.
`DebugUI::Initialize(window)` must be called after the OpenGL context is current.
Override `OnImGui()` in your application class to draw debug panels and overlays.

### App
The App module provides the application framework on top of the lower-level modules.
`Application` is the base class with a fixed-timestep physics loop and a rate-limited render loop (default 60 Hz physics, 144 Hz render).
Override `OnStart`, `OnFixedUpdate(dt)`, `OnUpdate(dt)`, `OnRender()`, and optionally `OnImGui()` / `OnShutdown()`.

`SystemRegistry` provides dependency-ordered, phase-based game system registration (`PreUpdate`, `FixedUpdate`,
`Update`, `PostUpdate`, `Render`) that an application wires up directly, alongside an `ActionMap` loaded from
`assets/game.json`.

`AppConfig` is loaded from `assets/game.json` and provides engine configuration (target frame rates, window settings, input bindings, etc.).

## Documentation
<!-- docs-block: start -->
- [Getting Started](<add-docs-link>)
- [Build & Tooling](<add-docs-link>)
- [Architecture Overview](<add-docs-link>)
- [API Reference](<add-docs-link>)
<!-- docs-block: end -->

## Useful Links
<!-- links-block: start -->
- [Roadmap](<add-link>)
- [Issue Tracker](<add-link>)
- [Community/Support](<add-link>)
<!-- links-block: end -->

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
