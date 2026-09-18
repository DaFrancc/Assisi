# A tour of the repository

The repository holds both the engine and a game built on it. You only work in
two places; the rest is the engine, and you can use it without reading it.

```text
Assisi/
├── apps/game/
│   ├── src/          ← your game's code
│   └── launch/       ← how the game starts (you rarely touch this)
├── assets/           ← your game's content: levels, models, textures, settings
├── modules/          ← the engine
├── tools/            ← build tools the engine uses
└── out/              ← everything the build produces
```

## `apps/game/src/`: your code

This is where your game's C++ goes. Right now it holds one example,
`DemoSystems.hpp` and `DemoSystems.cpp`, which makes objects spin, reacts to
key presses and spawns objects. Read it, change it, delete it when you're done
with it.

**You never have to register files anywhere.** Create a `.hpp` or `.cpp` file,
in `src/` or in any folder you make inside it, and the next build picks it up.
Delete a file and the next build forgets it. The build also scans every header
here for the engine's annotations. You'll meet them in
[Your first system](first-system.md).

Organize it however you like:

```text
apps/game/src/
├── Player/
│   ├── PlayerComponents.hpp
│   ├── PlayerSystems.hpp
│   └── PlayerSystems.cpp
└── Enemies/
    ├── EnemySystems.hpp
    └── EnemySystems.cpp
```

When one file includes another, write the path from `src/`, for example
`#include "Player/PlayerComponents.hpp"`.

## `apps/game/launch/`: how the game starts

This folder holds the program's entry points: the `main()` for the game, the
`main()` for the editor, their command-line options, and a headless server mode.

**You can ignore it.** It's there so you *can* change how your game starts if
you ever need to, but most games never will.

## `assets/`: your content

Everything your game loads at runtime lives here:

| Folder | What's in it |
|---|---|
| `levels/` | Levels (`.alvl`). You build these in the editor. |
| `blueprints/` | Blueprints (`.abp`): reusable objects you can place or spawn, like prefabs. |
| `models/`, `textures/`, `materials/` | Meshes, images and materials. |
| `config/` | Game settings: window title and size, the first level, input bindings. |
| `shaders/`, `editor/` | Used by the engine and editor. You can leave these alone. |

In code and in level files, assets are named by their path inside `assets/`,
for example `"levels/Test.alvl"` or `"blueprints/Bouncer.abp"`.

You'll see a `.aast` file next to every asset, like `Test.alvl.aast`. Each one
holds the asset's permanent ID, so that renaming a file doesn't break the levels
that use it. **The editor creates them for you** when it starts. Commit them to
git, and when you move or rename an asset, move its `.aast` with it.

## `modules/`: the engine

Each folder is one part of the engine, built as its own library. You'll include
headers from these in your code, like `<Assisi/ECS/Scene.hpp>`, but you don't
need to change them to make a game.

<details>
<summary>What each module does</summary>

| Module | What it does |
|---|---|
| `Core` | Logging, the asset file system, events, the job system, and reflection. |
| `Math` | Vectors, matrices and quaternions (built on GLM). |
| `Window` | The window, keyboard and mouse. |
| `Geometry` | Mesh and material data, and model import (glTF). |
| `Image` | Image loading. |
| `Render` | Drawing, with Vulkan. |
| `ECS` | Entities, components and queries. |
| `Runtime` | Built-in components like `Transform`, `Camera` and lights, and level loading/saving. |
| `Physics` | Rigid-body physics (Jolt). |
| `App` | The framework that ties it together: worlds, systems, levels, blueprints. |
| `Editor` | The level editor. |
| `Cook` | Prepares assets for shipping. |
| `Net`, `NetSync` | Networking and multiplayer. |
| `Debug` | Developer UI (Dear ImGui). |
| `Chiara` | The profiler. |

</details>

## `out/`: build output

Everything the build creates goes here, and it's safe to delete. The programs
you'll run are in `out/build/<build>/apps/game/`, for example
`out/build/gcc-dev/apps/game/Assisi-GameEditor`.

There are two programs, and they share all of your code:

- **`Assisi-GameEditor`** is your game with the editor built in. You'll use this
  one every day.
- **`Assisi-Game`** is your game alone, which is what players run. It isn't built
  by default; [Packaging your game](packaging.md) covers it.
