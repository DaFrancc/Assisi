# Your own components

The `Spin` system from the last chapter spins *everything*, all at the same
speed. Real games need per-object data: this crate spins fast, that one slowly,
the floor not at all. That data goes in a **component** you define yourself.

## Step 1: declare the component

Add a header, `apps/game/src/Tutorial/TutorialComponents.hpp`.

> **Don't worry about what goes inside `ACOMP(...)` and `AFIELD(...)` yet**,
> like the `min = 0` below. Copy it as it is for now; the
> [Options](#options) section later in this chapter explains them.

```cpp
#pragma once

#include <Assisi/Core/Reflect/Annotations.hpp>

/// Makes an entity spin around its vertical axis.
ACOMP()
struct Spinner
{
    AFIELD(min = 0) float radiansPerSecond = 1.f;
    AFIELD() bool clockwise = false;
};
```

- **`ACOMP()`** marks the struct as a component.
- **`AFIELD()`** marks each field the engine should know about. A field without
  `AFIELD()` still works in C++, but isn't saved, loaded or shown in the editor.
- **Give every field a default value.** It's what a newly added component starts
  with.

Just like systems, the build finds this header on its own.

## Step 2: use it in a system

Change the system to spin only entities that have a `Spinner`, each at its own
speed. In `TutorialSystems.cpp`:

```cpp
#include "Tutorial/TutorialComponents.hpp"

// ...

void SpinSystem(Assisi::App::SystemContext &ctx)
{
    using namespace Assisi;

    ECS::Scene &scene = ctx.world.scene;
    for (auto [entity, spinner, transform] : scene.QueryMut<Spinner, ECS::Transform>())
    {
        const float direction = spinner.Get().clockwise ? -1.f : 1.f;
        const float angle     = direction * spinner.Get().radiansPerSecond * ctx.dt;
        transform->rotation   = glm::angleAxis(angle, glm::vec3(0.f, 1.f, 0.f)) * transform->rotation;
    }
}
```

`spinner.Get()` reads the component without marking it changed, since this
system only reads it. `transform->` writes, which marks the `Transform` changed so
the engine moves the object.

**"Changed" applies to the whole component, not to individual fields.** The
engine doesn't track which field you touched, only whether the component was
touched with `->`. So once you use `->` on a component, it's marked changed, and
reading its other fields with `Get()` saves nothing: 99 reads through `Get()`
plus one `->` still marks the whole component. Within one component, just use
`->` everywhere. `Get()` only helps for a component you never write.

Notice how the loop above only uses `Get()` with `spinner` and only uses `->`
with `transform`. The `Spinner` component is never marked changed, but the
`Transform` component is.

## Step 3: add it in the editor

Build, open the editor, and select an object. In the inspector, click **Add
Component** and pick **Spinner**. Its two fields appear, ready to edit: a
number that can't go below 0, and a checkbox. Give a few objects different
speeds, press **Play**, and each spins at its own rate.

The component is saved with the level, so it's still there next time you open
it.

**All of this came from the annotations.** You didn't write any code to save,
load or display `Spinner`. The build's code generator, reflectgen, read the
header and wrote that code for you.

## Field types you can use

| Kind | Types |
|---|---|
| Numbers | `float`, `double`, `bool`, `int8_t` … `int64_t`, `uint8_t` … `uint64_t` |
| Math | `glm::vec2`, `glm::vec3`, `glm::vec4`, `glm::quat`, `glm::mat4` |
| Colors | `Math::Color3`, `Math::Color4` (shown as a color picker) |
| Text | `Core::ShortString` (up to 32 bytes), `Core::EntityName` (up to 64) |
| Other entities | `ECS::Entity` |
| Assets | `Core::AssetId` (a reference to a mesh, material or other asset) |
| Choices | Your own `enum class` marked `AENUM()` (shown as a dropdown) |
| Lists and maps | `std::vector`, `std::map`, `std::unordered_map` of the above |

Some common types are **not allowed**, and the build tells you what to use
instead:

| Instead of | Use |
|---|---|
| `int`, `unsigned` | `int32_t`, `uint32_t` |
| `long`, `short` | `int64_t`, `int16_t` |
| `char` | `int8_t` for a number, `Core::ShortString` for text |
| `std::string` | `Core::ShortString` or `Core::EntityName` |

This is so a level saved on one machine loads the same on another: `int` and
`long` can be different sizes on different platforms.

### Choices with an enum

```cpp
AENUM()
enum class Team : uint8_t
{
    Red,
    Blue,
};

ACOMP()
struct TeamMember
{
    AFIELD() Team team = Team::Red;
};
```

The enum must be in the **same header** as the component that uses it.

## Options

Put these inside `AFIELD(...)`:

| Option | What it does |
|---|---|
| `min = 0`, `max = 10` | Limits the value in the editor. A bound can also name another field: `max = outerAngle`. |
| `transient` | Not saved to the level. For values you compute at runtime. |

And inside `ACOMP(...)`:

| Option | What it does |
|---|---|
| `tracked` | The engine records when this component changes. You'll rarely need this yourself. |
| `replicable` | The component *can* be sent over the network. See [Multiplayer basics](multiplayer.md). |
| `transient` | Never saved at all: a marker or runtime-only component with no fields to store. |

<details>
<summary>Showing and hiding fields based on other fields</summary>

A field can be greyed out or hidden depending on the value of a `bool` or enum
field in the same component. That's how the physics component only shows
`radius` when the shape is a sphere:

```cpp
AFIELD(radioBroadcast) ColliderShape shape = ColliderShape::Box;
AFIELD(radioListen = {source = shape, value = Box, behavior = vanish})
glm::vec3 halfExtents{0.5f, 0.5f, 0.5f};
```

- `radioBroadcast` marks the field others depend on.
- `radioListen` says which field to watch (`source`), which value(s) make this
  field active (`value = Box`, or `value = {Sphere, Capsule}`), and what to do
  otherwise: `grey` or `vanish`.

</details>

<details>
<summary>Marker components with no fields</summary>

A component doesn't need fields. An empty one works as a label you can query
for:

```cpp
ACOMP()
struct Collectible
{
};
```

```cpp
for (auto [entity, collectible, transform] : scene.Query<Collectible, ECS::Transform>())
{
    Core::Log::Info("A collectible at height {}", transform.position.y);
}
```

</details>

## Rules to remember

- Components are **data only**. Put behavior in systems.
- Only types marked `ACOMP()` can be stored on entities. Adding a plain struct
  is an error.
- Component names must be unique across the game. Levels store components by
  their struct name, like `"Spinner"`, so renaming a component means levels that
  used the old name won't find it.
