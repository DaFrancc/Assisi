# Physics

Assisi simulates rigid bodies (boxes falling, balls bouncing, crates stacking)
with the [Jolt Physics](https://github.com/jrouwe/JoltPhysics) library. You
don't talk to Jolt directly. You describe bodies with a component and the engine
takes care of the rest.

## Making something physical

Give an entity a **`RigidBodyDescriptor`** (from
`<Assisi/Physics/PhysicsComponents.hpp>`) along with its `Transform`. In the
editor, that's **Add Component → RigidBodyDescriptor** on the selected object.

| Field | Meaning |
|---|---|
| `shape` | `Box`, `Sphere`, `Capsule` or `Cylinder`. |
| `halfExtents` | Half the box's size on each axis (for `Box`). |
| `radius` | For `Sphere`, `Capsule` and `Cylinder`. |
| `halfHeight` | For `Capsule` and `Cylinder`. |
| `isStatic` | `true` for things that never move, like floors and walls. |
| `enableCCD` | Continuous collision detection: stops fast, small objects from passing through thin walls. Costs a little more. |
| `channel`, `collidesWith` | Which collision group this body is in, and which groups it hits. Setting `channel` to `Trigger` makes a body that detects overlaps without blocking anything. |

The editor only shows the size fields that apply to the chosen shape.

When the level starts, the engine creates the real physics body. From then on
**physics owns the entity's position**: it falls, collides and comes to rest on
its own, and the engine copies the result into the `Transform` each step.

> A physics body's `Transform` belongs to physics while the game runs. Don't
> move it by writing to its `Transform`; use the calls below.

## Moving bodies from code

Each world has its own physics, at `ctx.world.physics`. You work with a body
through its **`RigidBody`** component, which the engine adds when it creates the
body.

```cpp
#include <Assisi/App/World.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

// Launch every body upward.
for (auto [entity, body] : scene.Query<Physics::RigidBody>())
{
    ctx.world.physics.SetBodyLinearVelocity(body, glm::vec3(0.f, 10.f, 0.f));
}
```

The calls you'll use most:

| Call | What it does |
|---|---|
| `SetBodyLinearVelocity(body, velocity)` | Sets how fast the body moves, in meters per second. |
| `GetBodyVelocity(body)` | Returns `{linear, angular}` velocity. |
| `GetBodyTransform(body)` | Returns `{position, rotation}` straight from physics. |
| `SetBodyTransform(body, position, rotation)` | Teleports the body. |
| `SetGravity(gravity)` | Changes gravity for the whole world. The default points down. |

**Put code that changes velocities in a `FixedUpdate` system**, so it runs in
step with physics and behaves the same at any frame rate.

> **Not yet available:** there's no function for applying a force or an
> impulse yet. To push a body, read its velocity, add to it, and set it back.

## Reacting to collisions

Every physics step, the world records which bodies started touching, stayed in
contact, or separated. Read them with `ContactEvents()`:

```cpp
for (const Physics::ContactEvent &contact : ctx.world.physics.ContactEvents())
{
    if (contact.phase == Physics::ContactPhase::Enter)
    {
        // contact.entity hit contact.other
    }
}
```

Each event has the two entities (`entity` and `other`), the contact `normal`,
the `velocity` at impact, the `phase` (`Enter`, `Stay` or `Exit`), and whether it
involved a trigger (`sensor`).

The list only covers the most recent physics step, so read it from a
**`PostFixedUpdate`** system (right after the step) or a **`FixedUpdate`** system
(just before the next one). Anything slower can miss events.

The engine's own `Bounce` system is a complete example: it reflects a body's
velocity when it hits something. Add the `Bounce` component to an object and the
`Bounce` system to your level to try it. Its source is in
`modules/App/src/PhysicsSystems.cpp`.

<details>
<summary>Ray casts and overlap tests</summary>

`PhysicsWorld` can also answer questions about the world:

- `CastRay(origin, sweep, filter, ...)` finds the first thing along a line, like
  a bullet or a line-of-sight check.
- `Overlap(shape, pose, ...)` lists the entities inside a shape, like an
  explosion radius.

See `modules/Physics/include/Assisi/Physics/PhysicsWorld.hpp` for their full
parameters.

</details>

<details>
<summary>Moving an object without physics</summary>

For something that moves on a fixed path, like a platform or a door, you don't
need a physics body. Write its `Transform` from a system, as in
[Your first system](first-system.md). The engine's built-in `Oscillator`
component and `Oscillate` system move an object back and forth this way.

</details>

<details>
<summary>Characters</summary>

The engine includes a character controller for player-like movement: walking,
jumping and crouching, with collision against the level. It's driven by the
`CharacterDescriptor` and `Character` components, and the `blueprints/Player.abp`
blueprint uses it. This part of the engine is still evolving, so it isn't
covered in detail here yet.

</details>
