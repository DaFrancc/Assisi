# Your own components

The `Spin` system from the last chapter spins *everything*, all at the same
speed. Real games need per-object data: this crate spins fast, that one slowly,
the floor not at all. That data goes in a **component** you define yourself.

## Try it: a spin speed for each object

### Step 1: declare the component

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

### Step 2: use it in a system

Change the system to spin only entities that have a `Spinner`, each at its own
speed. In `TutorialSystems.cpp`:

```cpp
#include "Tutorial/TutorialComponents.hpp"

// ...

void SpinSystem(Assisi::App::SystemContext &ctx)
{
    using namespace Assisi;

    ECS::Scene &scene = ctx.world.scene;
    for (auto [entity, spinner, transform] : scene.Query<Spinner, Mut<ECS::Transform>>())
    {
        const float direction = spinner.clockwise ? -1.f : 1.f;
        const float angle     = direction * spinner.radiansPerSecond * ctx.dt;
        transform.rotation    = glm::angleAxis(angle, glm::vec3(0.f, 1.f, 0.f)) * transform.rotation;
    }
}
```

`Spinner` is only read, so it's asked for plainly and comes back read-only.
`Mut<ECS::Transform>` comes back writable and marks each `Transform` changed so
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

### Step 3: add it in the editor

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
| Text | `Core::InternedString` (a name), `Core::DisplayedString` (words a player reads), `Core::PooledString` with one `Core::StringPool`, `Core::ShortString` (up to 32 bytes), `Core::EntityName` (up to 64). See [Strings](strings.md) |
| Other entities | `ECS::Entity` |
| Assets | `Core::AssetId` (a reference to a mesh, material or other asset) |
| Choices | Your own `enum class` marked `AENUM()` (shown as a dropdown) |
| Groups of fields | Your own struct marked `ASTRUCT()` (shown as a section you can open) |
| Lists and maps | `std::vector`, `std::map`, `std::unordered_map` of the above |
| Fixed-length lists | `std::array<T, N>` of the above |

Some common types are **not allowed**, and the build tells you what to use
instead:

| Instead of | Use |
|---|---|
| `int`, `unsigned` | `int32_t`, `uint32_t` |
| `long`, `short` | `int64_t`, `int16_t` |
| `char` | `int8_t` for a number, `Core::ShortString` for text |
| `std::string` | The string type for the job; see [Strings](strings.md) |

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

### Groups of fields with a struct

When several fields belong together, put them in a struct marked `ASTRUCT()`
and use it as a field:

```cpp
ASTRUCT()
struct Range
{
    AFIELD() float low = 0.f;
    AFIELD() float high = 1.f;
};

ACOMP()
struct Flicker
{
    AFIELD() Range brightness;
    AFIELD() std::vector<Range> steps;
};
```

- A struct can be a field on its own, or the element of a list, a map or an
  array. It can hold other structs.
- In a level file it's saved as an object of its own fields:
  `"brightness": { "low": 0.2, "high": 0.9 }`.
- In the editor it shows as a section you can open, and so does each struct
  in a list. An asset field inside a struct is typed as its path; the **...**
  browse button is only on a component's own asset fields.
- The struct must be in the **same header** as the component, or in a header it
  includes.
- `ASTRUCT()` takes no options. Inside the struct, `AFIELD(min = ..., max = ...)`
  and the radio options can only name the struct's own fields.
- A struct can't hold `ECS::Entity`, `ECS::InstanceId` or an `AFIELD(norep)`
  field, and it can't hold itself, even through a list. The build says so.

### Fixed-length arrays

`std::array<T, N>` holds exactly `N` values, and a level file holds it as a list
of exactly `N`:

```cpp
ACOMP()
struct LapTimes
{
    AFIELD() std::array<float, 3> bestSeconds{};
};
```

`N` may be a number or a named constant. A file whose list has a different
length is refused, rather than cut short or padded.

A C array, such as `float weights[3]`, is refused by default. The build names
the field and asks for `std::array<float, 3>`, which saves and loads the same
way. A project that needs C arrays can allow them by configuring with
`-DASSISI_FORBID_C_ARRAYS=OFF`. Even then, a `std::vector` or map of C arrays
is refused, because C++ can't store one.

### Lists in the editor

In the inspector, a list shows its name and how many rows it has; open it to
see the rows. Each row is edited the way a field of its type is, and a row
that is a struct opens to show its fields.

- **+ Add** puts a new row at the end, with the defaults your struct gives its
  fields. A struct row you add opens by itself.
- The arrows beside a row move it up or down, and **x** removes it.
- Each click is one step in the undo history, and so is each edit inside a
  row.
- A `std::array` edits its rows the same way but has no add, remove or move
  buttons, because its length is fixed.
- A map shows what it holds but can't be edited in the inspector yet.

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
| `requires = {A, B}` | Adding this component also adds `A` and `B`, with default values, if the entity doesn't have them. See below. |
| `excludes = {A}` | This component and `A` can't be on the same entity. See below. |

### Components that go together

Some components only make sense with others. `requires` lists the components
that must come along, and `excludes` lists the ones that can't share an entity:

```cpp
ACOMP()
struct Health
{
    AFIELD() float current = 100.f;
};

ACOMP(requires = {Health}, excludes = {ECS::Parent})
struct Enemy
{
};
```

Write each name as the plain struct name, without its namespace: `Parent`, not
`ECS::Parent`. The engine names components that way in level files, so two
components can't share a name.

With these rules:

- **Adding brings the requirements.** `scene.Add<Enemy>(entity)` also adds a
  default `Health`, in the same call, so no system ever sees an `Enemy` without
  one. A `Health` already on the entity is left as it is. A requirement's own
  requirements come along too.
- **Loading keeps saved values.** A level that saves both components loads the
  saved `Health`, not the default `Enemy` brought with it.
- **Excluded pairs are refused.** Adding a component that something on the
  entity excludes, or that excludes something already there, adds nothing. Code
  gets `nullptr`, an error in the log naming both components, and an assert in
  debug builds. The editor greys the component out in Add Component. A level or
  blueprint holding both keeps whichever it reads first, logs which one it
  dropped, and loads the rest. `scene.ConflictOf(entity, id)` asks the question without the error.
- **Removing is one way.** Removing `Enemy` leaves its `Health` in place.
  Removing `Health` while the `Enemy` is still there is refused, and the
  editor's remove button says which component needs it.

The build checks the rules themselves. A name that isn't a component, a
component that requires itself through a loop, and a component that could never
be added because it brings in something it excludes all stop the build.

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
