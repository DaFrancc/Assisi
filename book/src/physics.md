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
| `restitution` | How much speed an impact gives back: 0 stops dead, 1 loses nothing, above 1 gains speed on every bounce. Of two touching colliders, the bouncier one counts. Impacts slower than 1 m/s don't bounce, so a resting object stays at rest. |
| `channel`, `collidesWith` | Which collision channel this collider is on, and which channels it hits. See [Collision channels](#collision-channels). |
| `density` | Kilograms per cubic metre. With the shape's volume, how much this collider adds to its body's mass. Default 1000, the density of water. |
| `attach` | Under a `RigidBody`: `Piece` (default) makes it part of the body, `Body` makes it ride along as a body of its own. See [Colliders on child entities](#colliders-on-child-entities). |
| `enabled` | Unticked, the collider is taken out of the simulation without being removed: it has no body and nothing hits it. |
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
| `mass` | Kilograms. 0 adds up every collider of the body, each by its volume and `density`. Anything else scales that total to this. |
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

A trigger under a `RigidBody` rides along with it as a body of its own, because
a whole body is a sensor or none of it is. See
[Colliders on child entities](#colliders-on-child-entities).

### Colliders on child entities

A `RigidBody` or a `Character` cannot have a `Parent`: the simulation decides
where it is, so it cannot also be placed relative to another entity. The editor
refuses the combination. Anything parented *to* a body, like a camera or a
mesh, follows it as usual.

A `Collider` on a child entity takes one of three roles, decided by the first
`RigidBody` or `Character` above it in the hierarchy:

| Role | When | What it is |
|---|---|---|
| **Piece** | Under a `RigidBody`. | Part of that body's shape. It turns with the body and adds its volume times its `density` to the body's mass. |
| **Follower** | Under a `Character`; on the `Trigger` channel under a `RigidBody`; or `attach` set to `Body`. | A body of its own, put where its entity is after every step. It adds no mass and collides on its own channel and mask. |
| **Static** | Nothing above it moves. | Static geometry of its own, like any `Collider` without a `RigidBody`. |

The inspector says which one a collider is under its fields: "Piece of Crate",
"Follows Player", or "Static (no RigidBody)".

A part answers for its owner. A ray that hits one reports the owner as the
hit's `entity` and the part as its `piece`, and so does a contact event. Calls
given a part act on its owner: `Mass`, `BodyOf`, `GetBodyPose`,
`GetBodyState`, the forces and impulses, `IsTouching`, `Touching`, and a
query's `ignore`. `HasBody` is false for a part, since the body is its owner's.

A follower never touches or reports its own owner, or that owner's other
followers.

#### A crate with a handle

The crate is the body; the handle is a child with a `Collider` of its own:

```
Crate     Transform, RigidBody, Collider (Box 0.5)
└ Handle  Transform (0, 0.6, 0), Collider (Box 0.3 × 0.05 × 0.05)
```

The two shapes are one body. The crate falls, tips and comes to rest on
whichever part touches the floor. Moving the handle's `Transform` moves the
handle within the body, and the centre of mass moves with it. A `RigidBody`
with no `Collider` of its own and only child colliders is a body too, which is
how a table made of a top and four legs is built.

Giving the base a higher `density` makes the crate bottom-heavy, so it lands
the right way up. `RigidBody.mass` still sets an exact total when one is
wanted.

#### A character with a head

```
Player    Transform, Character
└ Head    Transform (0, 1.6, 0), Collider (Sphere 0.15, channel Hitbox)
```

The head is a follower: it goes wherever the player goes and does not get in
the player's way, because a character's own movement ignores its own
followers. Something falling onto the head is stopped by it, or not, by the
head's `collidesWith`.

#### A hitbox only shots find

The capsule a character moves with is a coarse shape. A shot can look for the
finer hitboxes instead by asking for the `Hitbox` channel only:

```cpp
const Physics::CollisionFilter shot{Core::Bitmask<Physics::CollisionChannel>::Of(Physics::CollisionChannel::Hitbox),
                                    Physics::CollisionChannel::Visibility};
std::optional<Physics::QueryHit> hit = world.CastRay(muzzle, aim * range, shot, shooter);
if (hit.has_value() && hit->piece == head)
{
    // A headshot on hit->entity.
}
```

Passing the shooter as `ignore` skips the shooter's own hitboxes. A shot asking
for `Character` only finds the capsules.

#### A trigger on a ball

```
Ball      Transform, RigidBody, Collider (Sphere 0.5)
└ Pickup  Transform, Collider (Sphere 1.5, channel Trigger)
```

The pickup radius follows the ball and reports what enters it as contact
events for the ball, with the pickup as the event's `piece`. The ball stays
solid: only the pickup's own body is a sensor. The pickup never reports the
ball itself.

#### Two bodies that should not collide

Channels and masks decide which kinds of thing collide. For one pair that
should not, like a projectile and the character that fired it, call
`IgnoreCollision(a, b)`; `IgnoreCollision(a, b, false)` undoes it. Naming a
part names its owner, so the exception covers every part of both. It lasts
until it is undone or either entity is destroyed.

### Collision channels

Every collider is on one **channel** and has a mask, `collidesWith`, of the
channels it collides with. Two colliders interact only when each one's mask
includes the other's channel, so either side can refuse the pair on its own.
Queries work the same way: a ray carries a channel and a mask too.

There are 32 channels. The engine has six of its own:

| Channel | For |
|---|---|
| `World` | Ordinary matter: floors, walls, props. |
| `Character` | Player and NPC capsules. |
| `Trigger` | Volumes that detect what enters them and block nothing. |
| `Visibility` | Line-of-sight and picking queries. |
| `Camera` | Camera collision queries. |
| `Hitbox` | The parts of a character a shot can hit. A convention only: the engine treats it like any other channel. |

The other 26 belong to your game. Take them with `GameChannel` and name them
in a header under `apps/game/src/`:

```cpp
#include <Assisi/Physics/CollisionChannelNames.hpp>

inline constexpr Assisi::Physics::CollisionChannel Bullet = Assisi::Physics::GameChannel(0);
inline constexpr Assisi::Physics::CollisionChannel Pickup = Assisi::Physics::GameChannel(1);

inline constexpr Assisi::Physics::ChannelName kChannelNames[] = {
    {"Bullet", Bullet},
    {"Pickup", Pickup},
};
ASSISI_COLLISION_CHANNEL_NAMES(kChannelNames);
```

Code uses the aliases, for example a ray that only hits pickups:

```cpp
using namespace Assisi;

const Physics::CollisionFilter pickupsOnly{Core::Bitmask<Physics::CollisionChannel>::Of(Pickup), Bullet};
std::optional<Physics::QueryHit> hit = world.CastRay(eye, forward * 3.f, pickupsOnly, player);
```

The editor shows your names in every channel dropdown and mask, and hides the
slots you haven't named. A level file saves a mask by those names too, as
`"collidesWith": ["World", "Bullet"]`, or `"collidesWith": "All"` for every
channel. If a mask has some of those hidden slots set and
others not, its heading says how many are set.

These mistakes fail to build:
- a `GameChannel` number past 25;
- two names for one slot, or one name for two slots;
- an empty name;
- a name for one of the engine's own channels.

A new collider collides with every channel, your game's included. So when you
start using a channel, everything already collides with it until you untick it
in the masks that shouldn't.

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
the way. See [What writing a `Transform` does](#what-writing-a-transform-does).

**Put code that moves bodies in a `FixedUpdate` system**, so it runs in step
with physics and behaves the same at any frame rate.

### Forces and impulses

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

### Velocity

A moving body's `BodyState` holds its `linearVelocity` (m/s), its
`angularVelocity` (rad/s), and whether it is `asleep`, as the last step left
them. Write a velocity to set it: the body takes it before the next step, and
wakes if it was asleep. A velocity written before the body exists, on the same
frame it is spawned, is the one it starts with.

```cpp
#include <Assisi/Physics/PhysicsComponents.hpp>

// Launch every body upward.
for (auto [entity, state] : scene.QueryMut<Physics::BodyState>())
{
    state.GetMut().linearVelocity = glm::vec3(0.f, 10.f, 0.f);
}
```

### Other calls

| Call | What it does |
|---|---|
| `GetBodyState(entity)` | The velocities and sleep, straight from physics rather than from the last step's `BodyState`. |
| `GetBodyPose(entity)` | The body's world `position` and `rotation`, straight from physics. |
| `Mass(entity)` | The body's mass in kilograms. 0 for a body nothing can push. |
| `HasBody(entity)` | Whether the entity has a body or a character of its own yet. False for a piece or a follower. |
| `BodyOf(entity)` | The entity whose body a collider answers for: itself, or the owner of a piece or a follower. |
| `IgnoreCollision(a, b)` | Stops two bodies colliding with or reporting each other. See [Two bodies that should not collide](#two-bodies-that-should-not-collide). |
| `SetGravity(gravity)` | Changes gravity for the whole world. The default points down. |

An entity added this step gets its body at the start of the next one. Call
`Reconcile()` to build it straight away, for example to cast a ray at it
before anything has stepped.

### Limits

`AppConfig::maxPhysicsBodies` sets how many bodies one world can hold. The
default is 65536. `AppConfig::maxFixedStepsPerFrame` (default 8) caps how many
physics steps one slow frame may run to catch up; any time beyond that is
dropped, so the game slows down instead of freezing.

## Reacting to collisions

Every physics step, the world records which bodies started touching and which
separated. To react, write a system that reads them with `ContactEvents()`.
Here a window breaks when a bullet hits it:

```cpp
ASYSTEM(PostFixedUpdate, name = "BreakOnHit") void BreakOnHitSystem(SystemContext &ctx)
{
    ECS::Scene &scene = ctx.world.scene;
    for (const Physics::ContactEvent &hit : ctx.world.physics.ContactEvents())
    {
        if (hit.phase != Physics::ContactPhase::Enter)
        {
            continue;
        }
        if (scene.Has<Window>(hit.entity) && scene.Has<Bullet>(hit.other))
        {
            scene.Destroy(hit.entity);
        }
    }
}
```

Each touching pair produces two events, one from each side, so a system only
has to look at the side it cares about. An event has:

| Field | Meaning |
|---|---|
| `entity`, `other` | The entity this event speaks for, and what it touched. |
| `phase` | `Enter` when they start touching, `Exit` when they stop. |
| `point` | Where they touch, in world space. |
| `normal` | Points away from `other`'s surface. |
| `velocity` | `entity`'s velocity just before the impact, before the solver slowed it. |
| `sensor` | Whether either side is a trigger. |
| `piece`, `otherPiece` | The entities whose colliders touched: the part of `entity`, and of `other`, that did. The same as `entity` and `other` for a body made of one collider. |

Give each behaviour its own small system that checks for its own component,
like `Window` above. Each one walks only that step's events, which are few.

The list only covers the most recent physics step, so read it from a
**`PostFixedUpdate`** system (right after the step) or a **`FixedUpdate`** system
(just before the next one). Anything slower can miss events.

### What is touching now

`IsTouching(a, b)` says whether two entities' bodies are touching, and
`Touching(entity, out)` lists everything one is touching. A trigger that opens
a door while a player stands in it can ask this every step instead of keeping
its own list from the events.

A resting pile of objects produces no events and costs nothing to track. If
you want an event every step for every pair that stays in contact, turn on
`Stay` events for that world with `SetStayEventsReported(true)`.

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
for (auto [entity, intent] : scene.QueryMut<Physics::CharacterIntent>())
{
    intent.GetMut().move = glm::vec3(1.f, 0.f, 0.f); // walk along +X
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
| `collidesWith` | all | Which collision channels the character hits. |

Changing a setting in the editor rebuilds the character in place.

<details>
<summary>Ray casts and overlap tests</summary>

`PhysicsWorld` can also answer questions about the world:

- `CastRay(origin, sweep, filter, ...)` finds the first thing along a line, like
  a bullet or a line-of-sight check.
- `CastShape(collider, pose, sweep, ...)` moves a collider along a line and finds
  the first thing it would hit.
- `Overlap(collider, pose, ...)` finds what a collider held still would overlap,
  like an explosion radius.

A shape query takes a `Collider` and uses its shape, its offset and its
collision groups, so it finds what that collider would touch there. Each query
also has an `All` form (`CastRayAll`, `CastShapeAll`, `OverlapAll`) that fills a
vector you pass in with every hit, nearest first.

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

