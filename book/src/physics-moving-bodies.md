# Moving bodies from code

There are four ways to move a dynamic body from code. Pick the one that says
what you mean:

| To | Do this |
|---|---|
| Push it: a thruster, wind, an explosion, a kick | Apply a force or an impulse. |
| Set how fast it moves | Write its `BodyState` velocity. |
| Put it somewhere and let it carry on | Write its `Transform`. It keeps its velocity. |
| Put it somewhere and stop it | Call `Teleport(entity, pose)`. |

Something that should follow a path you control every frame, like a lift or a
door, isn't a dynamic body pushed along. Make it `Kinematic` and write its
`Transform`: it then moves exactly where you put it and pushes whatever is in
the way. See
[What writing a `Transform` does](physics-scene-sync.md#what-writing-a-transform-does).

**Put code that moves bodies in a `FixedUpdate` system**, so it runs in step
with physics and behaves the same at any frame rate.

## Forces and impulses

Each world has its own physics, at `ctx.world.physics`. Its calls take the
entity the body belongs to:

```cpp
Physics::PhysicsWorld &physics = ctx.world.physics;

// Every fixed step the key is held: a steady push forward.
physics.AddForce(ship, glm::vec3(0.f, 0.f, -2000.f));

// Once: a kick upward.
physics.AddImpulse(crate, glm::vec3(0.f, 500.f, 0.f));
```

A **force** (newtons) pushes for one physics step. For a steady push, call
`AddForce` every `FixedUpdate`; stop calling it and the push stops. An
**impulse** (newton-seconds) is a single instant kick. Both change the body's
velocity by their amount divided by its mass, so a heavy body moves less than a
light one under the same push. `Mass(entity)` tells you the mass the
simulation uses.

| Call | What it does |
|---|---|
| `AddForce(entity, force)` | Pushes through the body's centre for the next step. |
| `AddForceAt(entity, force, point)` | Pushes at a world-space point, which spins the body too when the point is off-centre. |
| `AddImpulse(entity, impulse)` | Kicks through the body's centre. |
| `AddImpulseAt(entity, impulse, point)` | Kicks at a world-space point. |
| `AddTorque(entity, torque)` | Twists the body for the next step. |
| `AddAngularImpulse(entity, impulse)` | Spins the body at once. |
| `Wake(entity)` | Wakes a sleeping body. |
| `Sleep(entity)` | Stops the body where it is and puts it to sleep. It wakes when something touches or pushes it. |

These calls are requests. They're applied at the start of the next physics
step, in the order you made them. So they work on an entity spawned the same
frame, before it has a body. A push also wakes a sleeping body. Static and
kinematic bodies ignore pushes.

A character takes forces and impulses too, as a change to its velocity: an
impulse knocks it back, and a force pushes it along like wind. Its own movement
rules carry on from there, so ground friction slows a sideways shove. Torque,
`Wake`, `Sleep` and the "at a point" calls do nothing to a character.

## Velocity

A moving body's `BodyState` holds its `linearVelocity` (m/s), its
`angularVelocity` (rad/s), and whether it is `asleep`, as the last step left
them. Write a velocity to set it: the body takes it before the next step, and
wakes if it was asleep. A velocity written before the body exists, on the same
frame it is spawned, is the one it starts with.

```cpp
#include <Assisi/Physics/PhysicsComponents.hpp>

// Launch every body upward.
for (auto [entity, state] : scene.Query<Mut<Physics::BodyState>>())
{
    state.linearVelocity = glm::vec3(0.f, 10.f, 0.f);
}
```

## Reading a body and other calls

| Call | What it does |
|---|---|
| `GetBodyState(entity)` | The velocities and sleep, straight from physics rather than from the last step's `BodyState`. |
| `GetBodyPose(entity)` | The body's world `position` and `rotation`, straight from physics. |
| `Mass(entity)` | The body's mass in kilograms. 0 for a body nothing can push. |
| `HasBody(entity)` | Whether the entity has a body or a character of its own yet. False for a piece or a follower. |
| `BodyOf(entity)` | The entity whose body a collider answers for: itself, or the owner of a piece or a follower. |
| `IgnoreCollision(a, b)` | Stops two bodies colliding with or reporting each other. See [Excluding one pair of bodies from colliding](physics-child-colliders.md#excluding-one-pair-of-bodies-from-colliding). |
| `SetGravity(gravity)` | Changes gravity for the whole world. The default points down. |

An entity added this step gets its body at the start of the next one. Call
`Reconcile()` to build it straight away, for example to cast a ray at it
before anything has stepped.

## Body count and step limits

`AppConfig::maxPhysicsBodies` sets how many bodies one world can hold. The
default is 65536. `AppConfig::maxFixedStepsPerFrame` (default 8) caps how many
physics steps one slow frame may run to catch up; any time beyond that is
dropped, so the game slows down instead of freezing.

## Moving an object along a fixed path

For something that moves on a fixed path, like a platform or a door, write its
`Transform` from a system, as in [Your first system](first-system.md). The
engine's built-in `Oscillator` component and `Oscillate` system move an object
back and forth this way. Give it a `Collider` and a `Kinematic` `RigidBody` and
it also pushes what it meets and carries what stands on it.
