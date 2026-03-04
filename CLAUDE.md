# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build System

Assisi uses **CMake + Ninja** with **Conan 2** for dependency management. Build output goes to `out/build/<preset-name>/`.

### First-time setup (install Conan packages)
```bash
make init
```
This runs `conan install` for all three MSVC presets (debug, release, sanitize). Run this once before configuring CMake, or again after changing `conanfile.txt`.

### Configure
```bash
cmake --preset msvc-debug      # Windows Debug
cmake --preset msvc-release    # Windows Release
cmake --preset msvc-sanitize   # Windows + ASan
```
Linux presets: `gcc-debug`, `gcc-release`, `gcc-sanitize`, `clang-debug`, `clang-release`, `clang-sanitize`.

### Build
```bash
cmake --build --preset msvc-debug
cmake --build --preset msvc-debug --target Assisi-Sandbox
```

### Run the sandbox
```bash
./out/build/msvc-debug/apps/Sandbox/Assisi-Sandbox.exe
```

## Architecture

### Module layout
Each module under `modules/` is a static library with a CMake alias target `Assisi::<Name>`. All module targets call `Assisi_apply_defaults()`, which links the shared interface targets (Options, Warnings, Perf, Sanitize).

| Module | CMake target | Namespace | Responsibility |
|---|---|---|---|
| Core | `Assisi::Core` | `Assisi::Core` | AssetSystem, errors, `Prelude.hpp` (namespace fwd-decls) |
| Math | `Assisi::Math` | `Assisi::Math` | GLM wrapper/config |
| Window | `Assisi::Window` | `Assisi::Window` | GLFW lifecycle (`GlfwLibrary` singleton via `shared_ptr`), `WindowContext`, `WindowConfiguration` |
| Render | `Assisi::Render` | `Assisi::Render` | OpenGL backend: `RenderSystem`, `Shader`, `Buffer`, `DefaultResources`, `MeshBuffer` |
| ECS | `Assisi::ECS` | `Assisi::ECS` | Entity-Component-System (in progress) |
| Runtime | `Assisi::Runtime` | `Assisi::Runtime` | `WorldObject`, `Transform`, `Camera`, `SpawnSystem` — reusable game-logic prefabs |

`Assisi::Deps` is an interface target aggregating all third-party Conan libraries (GLFW, GLM, stb, Glad, Assimp).

### Dependency graph (simplified)
```
Sandbox → Render, ECS, Runtime, Window, Deps
Runtime → Core, Math, Render
Render  → Core, Math, Window, Deps
Window  → (GLFW via Deps)
```

### Key design patterns
- **RAII windowing**: `GlfwLibrary` is managed as a `shared_ptr`; `WindowContext` holds a `shared_ptr<GlfwLibrary>` to ensure GLFW is torn down after the last window.
- **Error handling**: C++23 `std::expected<T, AssetError>` is used throughout `AssetSystem` instead of exceptions.
- **Asset discovery**: `AssetSystem::Initialize()` checks the `ASSISI_ASSET_ROOT` env var first, then walks upward from CWD looking for an `assets/` directory.
- **Graphics backend abstraction**: `RenderSystem::Initialize(GraphicsBackend, WindowContext)` selects the backend at runtime; only OpenGL is implemented.
- **`WorldObject`**: Holds a non-owning pointer to `MeshBuffer` (must outlive the object) plus a transform and optional diffuse texture ID.

### Adding a new module
1. Create `modules/<Name>/CMakeLists.txt` following the existing pattern.
2. Call `Assisi_apply_defaults(Assisi-<Name>)` immediately after `add_library`.
3. Add `add_library(Assisi::<Name> ALIAS Assisi-<Name>)`.
4. Add `add_subdirectory(modules/<Name>)` in the root `CMakeLists.txt`.
5. Place public headers under `modules/<Name>/include/Assisi/<Name>/`.

## Code Style

Enforced via `.clang-format` (Microsoft base, Allman braces, 4-space indent, 120-column limit). Run `clang-format` before committing. Key conventions observed in the codebase:
- Private members prefixed with `_` (e.g., `_nativeWindowHandle`)
- `#pragma once` for include guards
- Namespaces match module name: `Assisi::Render`, `Assisi::Window`, etc.
- C++23 features are expected (`std::expected`, ranges, etc.)
- `NOMINMAX` and `WIN32_LEAN_AND_MEAN` are defined globally for MSVC builds via `Assisi::Options`