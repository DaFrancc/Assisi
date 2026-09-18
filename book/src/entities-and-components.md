# Entities and components

Assisi organizes a game with an **Entity–Component–System** design, or **ECS**
for short. It's the one idea you
need before anything else in this book, and it's simpler than it sounds.

## The three pieces

- An **entity** is a thing in your game: the player, a crate, a light, the
  camera. On its own an entity is just an ID with no data and no behavior.
- A **component** is a piece of data you attach to an entity. A `Transform` says
  where the entity is. A `MeshRenderer` says what it looks like. A
  `RigidBodyDescriptor` says it takes part in physics.
- A **system** is a function that runs every frame and does something to every
  entity that has certain components. A spinning system turns everything that
  has a `Transform`. A gravity system moves everything that has a rigid body.

So a crate isn't a `Crate` class. It's an entity with a `Transform`, a
`MeshRenderer` and a `RigidBodyDescriptor`. To make it glow, you don't subclass
it; you attach a light component.

> **Why do it this way?** Behavior comes from combining small pieces rather than
> from a class hierarchy, so you never get stuck deciding whether a
> `FlyingEnemy` should inherit from `Enemy` or from `Flyer`. It's also fast: the
> engine stores each kind of component together in memory, so a system that
> walks over thousands of them runs quickly.

## Where they live

When a level is loaded, it becomes a **world**. Each world has a **scene**: the
container that holds its entities and their components. When your system runs,
it gets the world it's running in, and reaches the scene through it:

```cpp
Assisi::ECS::Scene &scene = ctx.world.scene;
```

**Several worlds can exist at the same time, completely separately.** Each has
its own scene, its own physics and its own systems. An entity belongs to exactly
one world: a system running in one world only sees that world's entities, and
objects in two worlds never collide with each other. That's what lets a game
keep a menu alive behind a level, or load the next level while the current one
is still running.

Most of the time there's just one world, the level being played, and you don't
need to think about it. Code can also work across several worlds at once; that's
covered in [Multiple worlds](multiple-worlds.md), and most games never need it.

You'll see what `ctx` is in [the next chapter](first-system.md).

## The built-in components

The engine comes with the components most games need. You'll mostly add them
to entities in the editor, but it helps to know what they are.

| Component | Header | What it's for |
|---|---|---|
| `Transform` | `<Assisi/ECS/Transform.hpp>` | Position, rotation and scale. Almost every entity has one. |
| `Parent` | `<Assisi/Runtime/Hierarchy.hpp>` | Attaches an entity to another, so it moves with it. |
| `MeshRenderer` | `<Assisi/Runtime/Components.hpp>` | Draws a mesh with materials. |
| `Camera` | `<Assisi/Runtime/Components.hpp>` | A camera. The one with `isActive` set is the one you see through. |
| `DirectionalLight`, `PointLight`, `SpotLight` | `<Assisi/Runtime/LightComponents.hpp>` | Lights. |
| `RigidBodyDescriptor` | `<Assisi/Physics/PhysicsComponents.hpp>` | Makes the entity a physics body. See [Physics](physics.md). |
| `Skybox` | `<Assisi/Runtime/SkyComponents.hpp>` | A sky, lit by the sun. |
| `TimeOfDay`, `Sun`, `Moon` | `<Assisi/Runtime/TimeOfDay.hpp>` | A clock, a sun that moves with it, and a moon. |

`Transform` looks like this:

```cpp
struct Transform
{
    glm::vec3 position{0.f, 0.f, 0.f};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 scale{1.f, 1.f, 1.f};
};
```

### The sky, the sun and the moon

The sky, the sun and the moon are components too, and they all go on **one
entity**, the one with the `DirectionalLight`. That light *is* the sun.

| Add this | And you get |
|---|---|
| `DirectionalLight` | Sunlight from a fixed direction. On its own, it's just a light. |
| `Skybox` | A sky, colored by that light. Pick a **preset** to choose the look: `Clear`, `Arctic`, `Savanna`, `Tropical`, `Alpine`, `Hazy`, or `Airless` (black sky). `Custom` lets you set every value yourself. |
| `TimeOfDay` | A clock: the hour, the day, and how many real seconds a day lasts. |
| `Sun` | The sun follows the clock: it rises, crosses the sky and sets. The light's own `direction` is ignored. |
| `Moon` | A moon that rises, sets, goes through phases and lights the world at night. Needs a `Sun` on the same entity. |

For the clock to move while the game runs, add the **`TimeOfDay` system** to
the level's systems list. Without it the time stays where you set it, which is
fine for a level that's always noon.

The color and brightness of daylight come from the `DirectionalLight`, not the
`Skybox`. Change the light's color and the sky, the sun's disk and every lit
surface all follow.

The `Pretty` level (`assets/levels/Pretty.alvl`) is a working example to open in
the editor.

<details>
<summary>The main settings on each</summary>

**`TimeOfDay`**

| Field | Meaning |
|---|---|
| `hour` | Time of day, 0–24. |
| `dayLengthSeconds` | How long a full day lasts in real seconds. Default 600 (10 minutes). |
| `paused` | Stops the daily clock. |
| `dayOfYear`, `yearLengthDays`, `seasonsPaused` | The same for the year, which drives the seasons. |

**`Sun`**

| Field | Meaning |
|---|---|
| `latitudeDegrees` | Where on the planet the level is. It changes how high the sun gets. |
| `axialTiltDegrees` | The planet's tilt, which causes the seasons. `0` means no seasons. |
| `bearingDegrees` | Which way is east in your level, so you can choose where the sun rises. |

**`Moon`**

| Field | Meaning |
|---|---|
| `intensity`, `color` | How much the moon lights the world, and in what color. Real moonlight is far too dark to play in, so the default is brighter than reality. |
| `sizeDegrees`, `diskColor`, `diskIntensity` | How the moon looks in the sky. |
| `cycleDays`, `phaseAtEpoch` | The length of the phase cycle, and the phase it starts at. |

**`Skybox`**

| Field | Meaning |
|---|---|
| `preset` | The look. Every other sky field is greyed out unless this is `Custom`. |
| `minimumAmbient`, `minimumAmbientColor` | The least light the world ever gets. A moonless night is almost pitch black, which is realistic but often unplayable; raise this to keep night scenes readable. It doesn't cast shadows. `0` (the default) is off. |

</details>

## Working with the scene in code

These are the `Scene` calls you'll use most, from `<Assisi/ECS/Scene.hpp>`:

```cpp
using namespace Assisi;

ECS::Scene &scene = ctx.world.scene;

// Create an entity and give it components.
ECS::Entity crate = scene.Create();
(void)scene.Add<ECS::Transform>(crate, {.position = {0.f, 5.f, 0.f}});

// Read a component. Returns nullptr if the entity doesn't have one.
if (const ECS::Transform *t = scene.Get<ECS::Transform>(crate))
{
    Core::Log::Info("The crate is at height {}", t->position.y);
}

// Change a component: use GetMut, not Get (see below).
if (ECS::Transform *t = scene.GetMut<ECS::Transform>(crate))
{
    t->position.y += 1.f;
}

// Check for, and remove, a component.
if (scene.Has<ECS::Transform>(crate))
{
    scene.Remove<ECS::Transform>(crate);
}

// Mark the entity for destruction. It stays alive until the end of the frame.
scene.Destroy(crate);
```

`Add` returns a pointer to the new component, or `nullptr` if the entity already
has one. The `(void)` says you're deliberately ignoring it; the compiler warns
otherwise.

### Entity handles

`scene.Create()` returns an **`ECS::Entity`**: a **handle** to the entity, not
the entity itself. A handle is two small numbers, the entity's slot (`index`) and
which use of that slot it is (`generation`). It's cheap to copy, pass around and
store, including inside a component (`ECS::Entity` is an allowed field type), so
one entity can refer to another, like a turret pointing at its target.

Every `Scene` call takes a handle to say which entity you mean.

**A handle can outlive its entity.** When an entity is destroyed, its slot is
reused for the next one created, with a new `generation`. An old handle still
points at the slot but has the old generation, so the engine can tell it's
stale: `Get` returns `nullptr`, and `scene.IsAlive(handle)` returns `false`.
You'll never accidentally read some other entity through an old handle, but
check before you use one you've kept for a while:

```cpp
if (scene.IsAlive(target))
{
    // target still exists
}
```

#### "No entity": `NullEntity`

Sometimes you need a handle that points at nothing: a turret with no target yet,
or a search that found nothing. That's **`ECS::NullEntity`**. It uses a reserved
value that no real entity ever gets, so it can never be mistaken for one.

A handle converts to `bool`, and it's `false` only for `NullEntity`:

```cpp
ECS::Entity target = ECS::NullEntity; // no target yet

if (target)
{
    // target is set to *something*
}
```

`if (target)` only tells you the handle isn't `NullEntity`. It doesn't tell you
the entity still exists; for that, use `scene.IsAlive(target)`, which is also
`false` for `NullEntity`. So `IsAlive` alone covers both questions.

> **Always initialize handles to `NullEntity`.** A handle declared without a
> value, `ECS::Entity target;`, is **not** null: it's `{0, 0}`, which is the
> handle of the first entity the scene created. Code that forgets to set it will
> quietly act on that entity. The same goes for `ECS::Entity` fields in your own
> components: give them `= ECS::NullEntity`.
>
> ```cpp
> ACOMP()
> struct Turret
> {
>     AFIELD() ECS::Entity target = ECS::NullEntity;
> };
> ```

### Queries: every entity with these components

A system usually wants "every entity that has X and Y". That's a **query**:

```cpp
// Runs once for every entity that has both a Transform and a Camera.
for (auto [entity, transform, camera] : scene.Query<ECS::Transform, Runtime::Camera>())
{
    if (camera.isActive)
    {
        Core::Log::Info("The player's camera is at height {}", transform.position.y);
    }
}
```

Each loop gives you the entity and a reference to each component you asked for.

> **Every loop variable must be used**, or the build stops with an "unused"
> error. Warnings count as errors in this engine. If you don't need one, write
> `(void)entity;` in the loop.

`Without<>` skips entities that have some other component:

```cpp
// Every Transform, except entities that are physics bodies.
for (auto [entity, transform] :
     scene.Query<ECS::Transform>(ECS::Without<Physics::RigidBodyDescriptor>{}))
{
    Core::Log::Info("Entity {} has no physics body", entity.index);
}
```

### The one rule to remember: write with `GetMut`

Some components, including `Transform`, are **tracked**: the engine notices when
they change, and only updates what depends on them if they did. The engine only
sees a change made through **`GetMut`** (or `QueryMut`). If you write through
`Get` or a plain `Query`, the change is silently missed, and the object doesn't
visibly move.

**The compiler won't catch this yet.** `Get` and `Query` hand out writable
references, so writing through them compiles. The rule is simple: read with
`Get` and `Query`, write with `GetMut` and `QueryMut`. A future version of the
engine plans to make the wrong way a compile error.

There are two ways to change components you found with a query.

**Read with `Query`, then write with `GetMut`** only for the entities you
actually change:

```cpp
// Anything that fell below the floor goes back up to it.
for (auto [entity, transform] : scene.Query<ECS::Transform>())
{
    if (transform.position.y < 0.f)
    {
        if (ECS::Transform *t = scene.GetMut<ECS::Transform>(entity))
        {
            t->position.y = 0.f;
        }
    }
}
```

**Or use `QueryMut`**, which hands you each component ready to write:

```cpp
// Everything rises.
for (auto [entity, transform] : scene.QueryMut<ECS::Transform>())
{
    transform->position.y += 1.f * ctx.dt;
}
```

Which one to use depends on **how many entities you expect to change**:

- **Changing only some of them?** Use `Query`, check each entity, and call
  `GetMut` on the ones you change. Only those get marked as changed.
- **Changing most or all of them?** Use `QueryMut`.

It matters because the engine does work for every component marked as changed:
it updates the object for rendering, and in multiplayer it sends the change over
the network. Marking only what really changed keeps that work small.

<details>
<summary>Other rules for queries</summary>

- **Don't add or remove a component type you're querying while you loop over
  it.** For example, don't `Remove<Transform>` inside a loop over
  `Query<Transform>`. Collect the entities into a `std::vector` first, then change
  them after the loop. Debug builds assert on this; other builds misbehave
  silently.
- **Destroying entities inside a loop is fine.** `Destroy` only marks the
  entity, so it stays valid until the end of the frame.

</details>
