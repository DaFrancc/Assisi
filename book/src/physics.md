# Physics

Assisi simulates rigid bodies (boxes falling, balls bouncing, crates stacking)
with the [Jolt Physics](https://github.com/jrouwe/JoltPhysics) library. You
don't talk to Jolt directly. You describe bodies with components and the
engine takes care of the rest.

## Making something physical

The physics components are in `<Assisi/Physics/PhysicsComponents.hpp>`:

| Component | What it is |
|---|---|
| `Collider` | A shape, and what touching it is like. On its own, it is static geometry: a floor, a wall, a trigger volume that never moves. |
| `RigidBody` | Makes a `Collider` move. |
| `BodyState` | What a moving body is doing. Added with `RigidBody`. |
| `Character` | A player or an NPC. See [The character controller](#the-character-controller). |

**Static geometry is a `Collider` with no `RigidBody`.** Most of a level is
static, so most entities need only a `Collider`. In the editor, that's **Add
Component → Collider** on the selected object, and **Add Component →
RigidBody** to make it move.

### `Collider`

| Field | Meaning |
|---|---|
| `shape` | `Box`, `Sphere`, `Capsule` or `Cylinder`. |
| `halfExtents` | Half the box's size on each axis (for `Box`). |
| `radius` | For `Sphere`, `Capsule` and `Cylinder`. |
| `halfHeight` | For `Capsule` and `Cylinder`. |
| `offsetPosition`, `offsetRotation` | Move and turn the shape away from the entity's origin. |
| `friction` | How strongly a surface sliding across this one is held back. Default 0.2. |
| `restitution` | How much speed an impact gives back: 0 stops dead, 1 loses nothing. |
| `channel`, `collidesWith` | Which collision group this collider is in, and which groups it hits. |
| `collisionAsset` | Reserved for cooked collision shapes. Not used yet. |

The editor only shows the size fields that apply to the chosen shape.

The entity's `Transform.scale`, combined with its parents' scales, scales the
shape, and the offset scales with it. A sphere or a capsule has a single radius,
so it takes one scale on every axis; a cylinder takes one across its round
axes. Scaling one of those unevenly logs a warning, and the shape is built at
the nearest scale it can take.

### `RigidBody`

| Field | Meaning |
|---|---|
| `motion` | `Dynamic`: moved by gravity, collisions and velocity. `Kinematic`: moved only by writing its `Transform`, and it pushes dynamic bodies out of its way. |
| `mass` | Kilograms. 0 takes the mass from the shape's volume at the density of water. |
| `linearDamping`, `angularDamping` | Fraction of speed and spin lost per second. |
| `gravityScale` | Multiplies gravity for this body. 0 floats. |
| `lockedAxes` | Axes the body may not move along or turn about. Lock all three rotations to keep a body upright. |
| `ccd` | Continuous collision detection: stops fast, small objects from passing through thin walls. Costs a little more. |
| `allowSleep` | Whether the body may fall asleep when it comes to rest. A sleeping body costs nothing. |

A body that should stop moving for a while, like a held object, can be switched
to `Kinematic` and back. It keeps its mass and settings.

### Triggers

Setting a `Collider`'s `channel` to `Trigger` makes a volume that detects what
enters it and blocks nothing. A static trigger costs nothing while nothing awake
is near it, and notices a resting body only when that body wakes. A trigger with
a `Kinematic` `RigidBody` and `allowSleep` off notices bodies already at rest
inside it, including when the volume is moved onto them, at the cost of staying
awake.

A `RigidBody` or a `Character` cannot have a `Parent`: the simulation decides
where it is, so it cannot also be placed relative to another entity. The editor
refuses the combination. Anything parented *to* a body, like a camera or a
mesh, follows it as usual.

## How the scene and physics stay in step

Each world's physics follows its scene by itself. At the start of every step it
looks at what changed since the last one:

- An entity that gained a `Collider` gets a body. One that lost it, or was
  destroyed, loses its body.
- A changed `Collider` or `RigidBody` changes the live body. Adding or removing
  the `RigidBody` rebuilds it, as static or moving.
- A changed `Transform` moves the body there, and a changed scale resizes it.
- A velocity written to `BodyState` is given to the body.

After every physics step, the engine writes each moving body's pose into its
`Transform` and its velocities into its `BodyState`. A body at rest is left
alone, so a level full of sleeping objects costs nothing.

`Transform` is the simulation pose: what you read from it right after a step is
exactly where the body is. The picture on screen is smoothed separately. Each
frame, anything whose `Transform` was written during a fixed step is drawn
between its pose before that step and its pose after it, by how far the frame
is into the next step. Motion stays smooth on a display faster than the step
rate, and gameplay never sees the in-between pose. See
[Your first system](first-system.md#phases-when-a-system-runs).

A spot or point light that moves in fixed steps is drawn smoothly too, but by
default its shadow is redrawn only once per step, from where the step put the
light. The shadow can trail the light by up to one step, and a moving light
costs one shadow redraw per step rather than one per frame. To keep the shadow
exactly on the light, set `shadows.local.cache.redrawMovingLightsEveryFrame` to
`true` in `options.json`, or tick **Redraw Moving Lights Every Frame** in the
editor's shadow options.

### What writing a `Transform` does

You can move any physics object by writing its `Transform`. What happens next
depends on the kind of body:

| Body | What a `Transform` write does |
|---|---|
| Static | The collider is moved there, and anything resting on it wakes up. |
| Kinematic | The body is swept there over the next step, pushing what it meets and carrying what stands on it. |
| Dynamic | The body is placed there and keeps its velocity. |
| Character | Only a change of position moves it; turning it is the controller's job. |

To place a body and stop it as well, call `Teleport(entity, pose)`. A teleported
body is drawn at its new place at once, not slid there.

## Moving bodies from code

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

Each world also has its own physics, at `ctx.world.physics`. Its calls take the
entity the body belongs to:

| Call | What it does |
|---|---|
| `GetBodyVelocity(entity)` | Returns `{linear, angular}` velocity, straight from physics. |
| `GetBodyPose(entity)` | Returns the body's world `position` and `rotation`, straight from physics. |
| `Teleport(entity, pose)` | Places the body at a world pose and stops it. |
| `HasBody(entity)` | Whether the entity has a body or a character yet. |
| `SetGravity(gravity)` | Changes gravity for the whole world. The default points down. |

An entity added this step gets its body at the start of the next one. Call
`Reconcile()` to build it straight away, for example to cast a ray at it
before anything has stepped.

### Limits

`AppConfig::maxPhysicsBodies` sets how many bodies one world can hold. The
default is 65536. `AppConfig::maxFixedStepsPerFrame` (default 8) caps how many
physics steps one slow frame may run to catch up; any time beyond that is
dropped, so the game slows down instead of freezing.

**Put code that changes velocities in a `FixedUpdate` system**, so it runs in
step with physics and behaves the same at any frame rate.

> **Not yet available:** there's no function for applying a force or an
> impulse yet. To push a body, add to its `BodyState` velocity.

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

## The character controller

A player or an NPC is not a rigid body. It is a capsule the engine sweeps
through the level each step, with its own rules for walking, jumping and
crouching. Those rules follow Half-Life 2: the character keeps its momentum,
the ground slows it with friction, and steering adds speed along the direction
asked for.

### Setting one up

A character is three components:

| Component | What it is | Written by |
|---|---|---|
| `Character` | The controller's settings: its shape and how it moves. Saved with the level. | You, in the editor. |
| `CharacterIntent` | What the character is asked to do. | Your systems: the keyboard, an AI, or a network command. |
| `CharacterState` | What the last step did. | The engine, after every step. |

Give an entity a **`Character`** and a `Transform`; the other two come with it.
The entity's `Transform` is at the character's **feet**. The physics step reads
the intent and writes the state itself, so nothing else has to run for a
character to move. The `blueprints/Player.abp` blueprint is a complete example,
with a camera and keyboard and mouse control through the `CharacterLook`,
`CharacterInput` and `CharacterEye` systems.

A `Character` cannot also have a `Collider` or a `RigidBody`: it already is a
body.

### Telling it what to do

| `CharacterIntent` field | Meaning |
|---|---|
| `move` | Where to go, in world space. A unit vector asks for the walk speed; a shorter one asks for less. |
| `jump` | Set to `true` to ask for a jump. The step clears it. |
| `stance` | `Standing` or `Crouching`. Hold it for as long as it is wanted. |

```cpp
for (auto [entity, intent] : scene.Query<Mut<Physics::CharacterIntent>>())
{
    intent.move = glm::vec3(1.f, 0.f, 0.f); // walk along +X
}
```

`CharacterState` is the result of the last step: `velocity`, `ground`
(`OnGround`, `OnSteepGround`, `NotSupported` or `InAir`), `stance`, `canJump`,
`eyeHeight` and what the character is standing on.

### What happens each step

The step turns `move` into a **wish velocity**: the direction of `move`, with a
length of `walkSpeed` (times `crouchSpeedScale` while crouched). The character
is never set to that velocity. Each fixed step the controller does this, in
order:

1. **Stance.** The character changes to the stance asked for, if it fits.
2. **Jump.** A jump fires if one was asked for and the character may jump.
3. **Take-off limit.** On a step where a jump fires, the bunny-hop policy
   limits the horizontal speed.
4. **Friction.** On walkable ground, and not on a step where a jump fires, the
   speed drops by `max(speed, stopSpeed) * friction * dt`.
5. **Acceleration.** The speed *along the wish direction* rises toward the wish
   speed, by at most `acceleration * wishSpeed * dt`. Speed in any other
   direction is left alone.
6. **Gravity**, then the sweep through the level: sliding along walls, climbing
   steps and sticking to stairs on the way down.

Because friction and acceleration are separate, stopping is quick, and a change
of direction on the ground has a short slide while friction removes the old
velocity.

On a moving platform, friction and acceleration work relative to the platform,
so a character standing still rides along with it.

### Moving in the air

In the air there is no friction, and step 5 changes in one way: the wish speed
is cut down to `airWishSpeedCap` before the controller decides how much speed
may be added. The amount added per second still uses the full wish speed.

- Holding one direction adds speed only up to `airWishSpeedCap` in that
  direction, which is slow. A jump mostly keeps the velocity it started with.
- Turning the wish direction away from the current velocity keeps adding
  speed. Holding a strafe key and turning the view curves the jump and makes
  the character faster. This is air strafing.

### Bunny hopping

Friction only applies on the ground, and a character that jumps on the step it
lands never spends a step on the ground. It keeps the speed it gained in the
air, and a chain of such jumps keeps building speed. With the default
`jumpBufferTime`, pressing jump shortly before landing is enough to do this.

`bunnyHop` decides what happens to that speed at take-off:

| Value | Effect |
|---|---|
| `Allow` | Nothing is removed. Speed builds without limit. |
| `Cap` (default) | Horizontal speed is limited to `bunnyHopSpeedCap * walkSpeed`. |
| `Disallow` | Horizontal speed is limited to `walkSpeed`, so hopping gains nothing. |
| `Boost` | Half-Life 2's own rule, which allows accelerated back hopping. See below. |

#### Accelerated back hopping

`Boost` copies what Half-Life 2 does on a jump, including the bug that
speedrunners use. It is off unless you choose it.

On each jump the controller:

1. Works out a boost: half of the forward speed being asked for (a tenth while
   crouched).
2. Sets a limit: `walkSpeed` plus half of it (plus a tenth while crouched).
3. If the current speed plus the boost is over the limit, takes the excess out
   of the boost. A character already over the limit ends up with a negative
   boost.
4. Adds the boost along the direction the character is **facing**.

Facing the way it travels, a character over the limit is slowed down to it.
Facing the other way, the same negative boost pushes it along its travel, so it
speeds up. To back hop: get over the limit, turn around, let go of the movement
keys and jump each time you land. Every jump adds the excess again. Crouching
lowers the limit, so it gains more.

The facing comes from the character's `Transform`, which the step reads each
time.

### Crouching and the crouch jump

On the ground, crouching keeps the feet in place and lowers the head. Standing
up is refused while something is in the way overhead, and the character stands
by itself once it is clear.

In the air it is the other way round: crouching keeps the head in place and
**lifts the feet** by the difference between the two heights. Crouching during
a jump therefore clears a ledge that the jump alone does not. Standing up in the
air lowers the feet again, and is refused while the ground is too close below.

The `CharacterEye` system keeps a camera parented to the character at the
character's eye height, after every step, and the camera is smoothed between
steps like everything else. The eye eases between `eyeHeight` and
`crouchEyeHeight` at `eyeSpeed`. When the feet move in the air the eye height
changes by the same amount at once, so the view stays where it was.

### The movement settings

Distances are in meters and times in seconds. The defaults are Half-Life 2's
values, converted at 1.905 cm to one of its units.

| Field | Default | Meaning |
|---|---|---|
| `walkSpeed` | 3.62 | The speed asked for with a full `move`. It is not a hard limit. Half-Life 2's normal run, 190 units a second. |
| `crouchSpeedScale` | 0.333 | Multiplies `walkSpeed` while crouched. |
| `friction` | 4 | Fraction of the speed lost per second on the ground. Half-Life 2's `sv_friction`. |
| `stopSpeed` | 1.905 | Below this speed, friction acts as if the character were moving this fast, so the last of the speed goes quickly. `sv_stopspeed` 100. |
| `groundAcceleration` | 10 | Speed gained per second on the ground, as a multiple of the wish speed. `sv_accelerate`. |
| `airAcceleration` | 10 | The same in the air. `sv_airaccelerate`. Zero means no steering in the air. |
| `airWishSpeedCap` | 0.5715 | The wish speed limit used in the air. 30 units a second. |
| `bunnyHop` | `Cap` | What a jump on the landing step keeps. See above. |
| `bunnyHopSpeedCap` | 1.7 | The limit for `Cap`, as a multiple of `walkSpeed`. |
| `jumpSpeed` | 3.05 | Upward speed at the start of a jump. Rises about 0.4 m. |
| `gravityScale` | 1.165 | Multiplies the world's gravity for this character. |
| `coyoteTime` | 0.1 | How long after walking off a ledge a jump still works. |
| `jumpBufferTime` | 0.15 | How long before landing a jump request is remembered. |

### The shape settings

| Field | Default | Meaning |
|---|---|---|
| `radius` | 0.3 | Half the character's width. |
| `halfHeight` | 0.6 | Half the height of the capsule's straight part. The character is `2 * (halfHeight + radius)` tall. |
| `crouchHalfHeight` | 0.3 | The same while crouched. |
| `eyeHeight`, `crouchEyeHeight` | 1.5, 0.9 | Height of the eye above the feet in each stance. |
| `eyeSpeed` | 3 | How fast the eye moves between the two heights. |
| `maxSlopeDegrees` | 50 | Steeper ground cannot be stood on; the character slides. |
| `maxStepHeight` | 0.4 | The tallest step climbed without jumping. |
| `mass`, `pushStrength` | 70, 100 | How the character pushes bodies and is pushed. |
| `canPushBodies`, `canBePushed` | `true` | Turn either direction of pushing off. |
| `collidesWith` | all | Which collision groups the character hits. |

Changing a setting in the editor rebuilds the character in place.

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

For something that moves on a fixed path, like a platform or a door, write its
`Transform` from a system, as in [Your first system](first-system.md). The
engine's built-in `Oscillator` component and `Oscillate` system move an object
back and forth this way. Give it a `Collider` and a `Kinematic` `RigidBody` and
it also pushes what it meets and carries what stands on it.

</details>

