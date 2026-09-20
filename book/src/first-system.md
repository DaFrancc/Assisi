# Your first system

In this chapter you'll write a system, build it, and watch it run in the editor.

A **system** is a plain C++ function that the engine calls for you, for example
once per frame. You declare it with the `ASYSTEM` annotation, and a level turns
it on by naming it.

## Step 1: create the files

Make a new folder `apps/game/src/Tutorial/` with two files in it.

> **Don't worry about what this code means yet.** Copy it as it is for now.
> Once you've seen it run, the rest of this chapter explains the `ASYSTEM` line
> and `ctx`. The loop is the `QueryMut` from
> [Entities and components](entities-and-components.md).

`apps/game/src/Tutorial/TutorialSystems.hpp`:

```cpp
#pragma once

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

/// Spins every object around its vertical axis.
ASYSTEM(Update, name = "Spin")
void SpinSystem(Assisi::App::SystemContext &ctx);
```

`apps/game/src/Tutorial/TutorialSystems.cpp`:

```cpp
#include "Tutorial/TutorialSystems.hpp"

#include <Assisi/App/World.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

void SpinSystem(Assisi::App::SystemContext &ctx)
{
    using namespace Assisi;

    constexpr float kRadiansPerSecond = 1.f;

    // ctx.dt is the time since last frame, so the speed is the same at any frame rate.
    const glm::quat step = glm::angleAxis(kRadiansPerSecond * ctx.dt, glm::vec3(0.f, 1.f, 0.f));

    ECS::Scene &scene = ctx.world.scene;
    // Physics bodies are moved by physics; leave them alone.
    for (auto [entity, transform] :
         scene.QueryMut<ECS::Transform>(ECS::Without<Physics::RigidBodyDescriptor>{}))
    {
        transform->rotation = step * transform->rotation;
    }
}
```

**That's all.** There's no list of files to update and no registration call to
write. The build finds both files because they're in `src/`, and finds the
system because of the `ASYSTEM` annotation.

## Step 2: build

Use the same build command as in [Installation](installation.md). For example,
on Linux with GCC:

```bash
make gcc-dev
```

On Windows:

```bash
make msvc-dev
```

## Step 3: turn it on in a level

A system only runs in levels that list it. That way a menu level doesn't run
your gameplay code, and you can switch behavior on and off per level.

Open the editor with a level (`-l levels/Test.alvl`) and find the **Systems**
panel. It lists the systems the level runs. Type `Spin` in its **Add System**
box, press **Enter**, then **Save** the level in the **Levels** panel.

Press **F5** (or **Run** in the **Game** panel) to play: everything without
physics starts spinning, which may include the camera and the lights. **F7**
stops. The next chapter fixes the "everything" part, so that only the objects
you choose spin.

If `Spin` doesn't appear in the Add System box, the build didn't pick up the
system. Check that the header is under `apps/game/src/` and that you rebuilt.

<details>
<summary>What the level file looks like</summary>

A level is a JSON file in `assets/levels/`. Its `systems` list is plain text,
and editing it by hand works too:

```json
{
  "systems": [
    "Spin",
    "CaptureCursor"
  ],
  ...
}
```

The names are the ones you gave with `name = "..."`, not the C++ function names.

</details>

## What `ASYSTEM` means

```cpp
ASYSTEM(Update, name = "Spin")
void SpinSystem(Assisi::App::SystemContext &ctx);
```

- **`Update`** is the **phase**: *when* the system runs. `Update` runs once per
  frame. The full list is below.
- **`name = "Spin"`** is what levels call it. It's required, and must be unique
  across the whole game.
- The function must be a free function (not a class method or a lambda) that
  returns `void` and takes `SystemContext &`.
- It must be *declared in a header* inside `src/`. The build reads headers to
  find systems.

`ASYSTEM` is invisible to the compiler: it expands to nothing. A tool called
**reflectgen** reads your headers during the build and generates the code that
registers the system. If you make a mistake in an annotation, it's reflectgen
that stops the build and tells you what's wrong.

## What's in `ctx`

The `SystemContext` your system receives holds everything it can reach:

| Field | What it is |
|---|---|
| `ctx.world` | The world (level) this system is running in. `ctx.world.scene` holds its entities; `ctx.world.physics` is its physics. |
| `ctx.dt` | Seconds since the last run. In `FixedUpdate` it's the fixed step (1/60 s by default). |
| `ctx.simTick` | How many fixed steps have run. Useful as a clock. |
| `ctx.input` | The keyboard and mouse. **Can be `nullptr`**: see [Input](input.md). |
| `ctx.actions` | Named input actions like "Jump". See [Input](input.md). |
| `ctx.worldManager` | Manages every loaded world. Use `ctx.worldManager->RequestTravel("levels/Other.alvl")` to change level. |
| `ctx.isActiveWorld` | Whether this is the world being shown and played. |

## Phases: when a system runs

| Phase | When |
|---|---|
| `Begin` | Once, when the level starts. For setup, like spawning the player. |
| `Loaded` | Once, when all of the level's assets have finished loading. |
| `PreUpdate` | Every frame, after input is read, before game logic. |
| `FixedUpdate` | At a fixed rate (60 per second by default), **before** physics. Anything that pushes physics objects goes here. |
| `PostFixedUpdate` | At the fixed rate, right **after** physics. For reacting to what physics just did. |
| `Update` | Every frame. **Most game logic goes here.** |
| `PostUpdate` | Every frame, after `Update`. For cleanup. |

`FixedUpdate` can run zero, one or several times in a frame, depending on how
fast the game is running. Use it for physics-related code so it behaves the same
at any frame rate.

## Options

You can add more options after the name:

```cpp
ASYSTEM(Update, name = "Shoot", after = "Aim", activeWorldOnly)
void ShootSystem(Assisi::App::SystemContext &ctx);
```

- **`after = "Aim"`** / **`before = "Aim"`** make this system run after (or
  before) the system named `Aim` in the same phase. Without these, don't assume
  any order between systems. The name is quoted, like `name` itself: it's the
  system's name, not a C++ function, and a bare one won't build.

  To order a system against several others, write `after =` or `before =` once
  for each:

  ```cpp
  ASYSTEM(Update, name = "Shoot", after = "Aim", after = "Reload", before = "Recoil")
  void ShootSystem(Assisi::App::SystemContext &ctx);
  ```

  `Shoot` then runs after both `Aim` and `Reload`, and before `Recoil`.
- **`activeWorldOnly`** only runs the system in the world that's being shown and
  played. Use it for anything that reads input or controls the camera or HUD.
  When several worlds are loaded, only one should react to the player's keys.

## Tips

- **Keep systems stateless.** Don't store anything in `static` or global
  variables. Put state in components instead. The same system can run in
  several worlds at once, and they'd share the variable.
- **Log with** `Assisi::Core::Log::Info("value is {}", x);` from
  `<Assisi/Core/Logger.hpp>`. It uses `std::format` syntax. Output goes to the
  terminal and to a log file.
- The example game in `apps/game/src/DemoSystems.cpp` has more systems to learn
  from.

Next, you'll give your objects their own data with
[your own components](your-own-components.md).
