# The character controller

A player or an NPC needs different physics from a box. It should walk up
stairs, stay upright, stop when the player lets go of the keys, and never be
knocked over. A `Character` component does this.

A character's collision is a capsule (a cylinder with rounded ends). Each step,
the engine moves the capsule through the level by the character's own rules:
it slides along walls, climbs steps and stays on slopes. How it speeds up,
slows down and steers in the air is chosen by its **movement** component:

- **`ArcadeMovement`** speeds the character up and slows it down at a steady
  rate, straight towards the velocity it is asked for. It never goes faster
  than it asks to. `blueprints/Player.abp` uses this one.
- **`SourceMovement`** moves it the way Half-Life 2 does: ground friction slows
  it, speed is added only along the direction asked for, and a player can gain
  speed in the air by turning while strafing.

## Character components

A character is made of these components:

| Component | What it is | Written by |
|---|---|---|
| `Character` | The capsule: its size, the slopes and steps it can climb, its eye height, and how it pushes and is pushed. | You, in the editor. |
| `CharacterLocomotion` | How fast it walks, how high it jumps, and how strongly gravity pulls it. Both movements read it. | You, in the editor. |
| `ArcadeMovement` or `SourceMovement` | How it speeds up, slows down and steers in the air. One or the other, never both. | You, in the editor. |
| `CharacterIntent` | What the character is asked to do. | Your systems: the keyboard, an AI, or a network command. |
| `CharacterState` | What the last step did. | The engine, after every step. |

Give an entity a `Transform` and an **`ArcadeMovement`** or a
**`SourceMovement`**; the other components come with it. The entity's
`Transform` is at the character's **feet**. The physics step reads the intent
and writes the state itself, so nothing else has to run for a character to
move. The `blueprints/Player.abp` blueprint is a complete example, with a camera
and keyboard and mouse control; [Players](players.md) explains how a person's
controls reach it.

To change a character's movement, remove one movement component and add the
other. This works in the editor and from code, also while the game runs; the
character keeps its speed and changes how it steers from the next step. To
remove a character, remove its movement component first: the `Character` can't
go while a movement component needs it.

A `Character` with no movement component doesn't walk or jump. On the ground it
stays still on whatever it stands on; in the air it keeps its speed and falls at
the world's gravity.

A `Character` cannot also have a `Collider` or a `RigidBody`: it already is a
body.

## Driving a character with `CharacterIntent`

| `CharacterIntent` field | Meaning |
|---|---|
| `move` | Where to go, in world space. A unit vector asks for the walk speed; a shorter one asks for less. |
| `jump` | Set to `true` to ask for a jump. The step clears it. |
| `stance` | `Standing` or `Crouching`. Hold it for as long as it is wanted. |
| `speedScale` | Multiplies the walk speed, whatever the `CharacterLocomotion` preset says. 1 by default; above 1 to sprint, below to slow down. Hold it for as long as it is wanted. |

```cpp
for (auto [entity, intent] : scene.Query<Mut<Physics::CharacterIntent>>())
{
    intent.move = glm::vec3(1.f, 0.f, 0.f); // walk along +X
}
```

### Example: sprinting

The sample game's `Sprint` system makes a player run 1.6 times as fast while
Left Shift is held. It takes a player action for the `Sprint` input action (see
[Your own actions](players.md#your-own-actions)) and sets `speedScale` from it:

```cpp
ASYSTEM(FixedUpdate, name = "Sprint", before = "CharacterCommand")
void SprintSystem(Assisi::App::SystemContext &ctx)
{
    for (auto [entity, controls, intent] :
         ctx.world.scene.Query<Assisi::App::PlayerInput, Mut<Assisi::Physics::CharacterIntent>>())
    {
        intent.speedScale = controls.held.Has(Sprint) ? 1.6f : 1.f;
    }
}
```

`blueprints/Player.abp` lists `Sprint` among its systems. A slowing effect or an
NPC that should walk calmly sets `speedScale` the same way. In a networked game
nothing more is needed: a client that steps its character again to correct a
prediction uses the `speedScale` each step had, not the one it has now.

`CharacterState` is the result of the last step: `velocity`, `ground`
(`OnGround`, `OnSteepGround`, `NotSupported` or `InAir`), `stance`, `canJump`,
`eyeHeight`, what the character is standing on (`groundEntity`) and the carrier
it is riding (`baseEntity`).

## Walk speed and jump: `CharacterLocomotion`

The step turns `move` into a **wish velocity**: the direction of `move`, with a
length of `walkSpeed` times the length of `move` (and times `crouchSpeedScale`
while crouched). Both movements steer towards the wish velocity; they differ in
how.

`preset` picks the walk speed, crouch speed, jump speed and gravity scale
together. While it is anything but `Custom`, those four fields are greyed out in
the editor and the preset's numbers are used instead.

| `preset` | `walkSpeed` | `crouchSpeedScale` | `jumpSpeed` | `gravityScale` | Feels like |
|---|---|---|---|---|---|
| `Source` (default) | 3.62 | 0.333 | 3.05 | 1.165 | A brisk run and a short, heavy jump that rises about 0.4 m. Made for `SourceMovement`. |
| `Arcade` | 5 | 0.4 | 4.5 | 1 | Faster, with a higher jump that rises about 1 m. Made for `ArcadeMovement`. |
| `Custom` | the fields | the fields | the fields | the fields | Whatever you type. |

Distances are in meters and times in seconds.

| Field | Default | Meaning |
|---|---|---|
| `walkSpeed` | 3.62 | The speed asked for with a full `move`. |
| `crouchSpeedScale` | 0.333 | Multiplies `walkSpeed` while crouched. |
| `jumpSpeed` | 3.05 | Upward speed at the start of a jump. |
| `gravityScale` | 1.165 | Multiplies the world's gravity for this character. |
| `coyoteTime` | 0.1 | How long after walking off a ledge a jump still works. Not set by the preset. |
| `jumpBufferTime` | 0.15 | How long before landing a jump request is remembered. Not set by the preset. |
| `maxFallSpeed` | 0 | The fastest the character falls, in m/s. Zero sets no limit. Not set by the preset. |

On a moving platform, both movements steer relative to the platform, so a
character standing still rides along with it.

## `ArcadeMovement`

Each step, the character's horizontal velocity moves straight towards the wish
velocity by at most `acceleration * dt`, and stops exactly on it. When nothing
is asked for, it slows to a stop at the **braking** rate instead. The character
never goes faster than it asks to.

On the ground, `turnFriction` first swings the velocity round towards the
direction asked for while keeping its speed. With it at zero, a turn is left to
`groundAcceleration`, which slows the character through the turn.

All rates are in m/s², except `turnFriction`.

| Field | Default | Meaning |
|---|---|---|
| `groundAcceleration` | 60 | How fast it speeds up on the ground. At 60 it reaches a walk speed of 5 m/s in under a tenth of a second, which feels instant. Lower it to make the character feel heavy. |
| `brakingDeceleration` | 60 | How fast it slows down on the ground once the keys are let go. Lower it for a slide to a stop, and much lower for ice. |
| `turnFriction` | 0 | How quickly it turns to a new direction without losing speed, per second. At 10, a tenth of a second makes up about two thirds of a turn. |
| `airAcceleration` | 10 | How fast it steers in the air. Zero means the character can't steer once it has jumped. |
| `airBrakingDeceleration` | 10 | How fast it slows down in the air once the keys are let go. Zero keeps all the momentum it jumped or fell with. |

## `SourceMovement`

Each fixed step, a `SourceMovement` character does this, in order:

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
velocity. `walkSpeed` is not a hard limit: speed gained in the air or carried
off a moving platform is kept until friction takes it.

The defaults are Half-Life 2's values, converted at 1.905 cm to one of its
units.

| Field | Default | Meaning |
|---|---|---|
| `friction` | 4 | Fraction of the speed lost per second on the ground. Half-Life 2's `sv_friction`. |
| `stopSpeed` | 1.905 | Below this speed, friction acts as if the character were moving this fast, so the last of the speed goes quickly. `sv_stopspeed` 100. |
| `groundAcceleration` | 10 | Speed gained per second on the ground, as a multiple of the wish speed. `sv_accelerate`. |
| `airAcceleration` | 10 | The same in the air. `sv_airaccelerate`. Zero means no steering in the air. |
| `airWishSpeedCap` | 0.5715 | The wish speed limit used in the air. 30 units a second. |
| `bunnyHop` | `Cap` | What a jump on the landing step keeps. See below. |
| `bunnyHopSpeedCap` | 1.7 | The limit for `Cap`, as a multiple of `walkSpeed`. |

### Air movement and air strafing

In the air there is no friction, and step 5 changes in one way: the wish speed
is cut down to `airWishSpeedCap` before the controller decides how much speed
may be added. The amount added per second still uses the full wish speed.

- Holding one direction adds speed only up to `airWishSpeedCap` in that
  direction, which is slow. A jump mostly keeps the velocity it started with.
- Turning the wish direction away from the current velocity keeps adding
  speed. Holding a strafe key and turning the view curves the jump and makes
  the character faster. This is air strafing.

### Jumping on the landing step (bunny hopping)

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

### Accelerated back hopping

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

## Riding a moving body

Taking the platform's velocity is enough for a lift or a slow, flat platform.
It is not enough for a boat that rocks or a train that turns: the velocity a
character takes is a step old, so it slides across the deck, and on a sharp turn
it is thrown off.

For those, make the body a **carrier**: give it a `Carrier` component, beside
its `RigidBody`. A character on a carrier **rides** it, and the carrier is its
**base**:

- Each step, the character is first moved with its base, by exactly the
  distance and the turn the base made. Then it moves by its own intent, which
  is worked out relative to the base. A character standing still on a
  spinning deck stays on the same spot of the deck.
- The character's facing turns with the base's yaw, so a player looking at the
  bow keeps looking at the bow. Only the yaw: on a rocking deck the character
  stays upright and the view does not roll.
- `CharacterState.baseEntity` names the base. `velocity` and `groundVelocity`
  stay world velocities, with the base's motion included.
- When the ride ends, the character keeps its world velocity, so a jump off a
  moving boat carries the boat's speed. Writing the character's `Transform`
  always ends the ride.

The `Carrier` decides when a ride starts and ends:

| Field | Default | Meaning |
|---|---|---|
| `join` | `Touch` | What starts a ride: `Touch` or `Volume`, below. |
| `hold` | `Touch` | What keeps a ride going. A jump leaves the deck, so with `Touch` it ends the ride unless the grace time covers it. |
| `footing` | `Ground` | What a rider the carrier holds moves with while it stands on ground that is not the carrier — possible when a volume holds it. `Ground`: that ground, as any character does, so the carrier sweeps past it and the ride ends once the volume has gone by. `Carrier`: the carrier, so the rider stays still in its frame and slides over the ground. |
| `graceTime` | 0 | Seconds a rider is kept after `hold` stops holding it. Standing on anything else ends the ride at once. |
| `grip` | 1 | How much of the carrier's speed a boarding rider takes up. See below. |
| `priority` | 0 | When two carriers would take the same rider, the higher one does, even from a carrier the rider is already on. On a tie the rider keeps the one it is on. A ship landing in a hangar that is itself a carrier can take its crew from the hangar this way. |

`join` and `hold` each take one of two values, and the second includes the
first:

- **`Touch`**: standing on one of the carrier's colliders. Brushing its side is
  not enough.
- **`Volume`**: `Touch`, or being inside one of the carrier's volumes, whether
  standing, jumping or falling.

A **volume** is a trigger under the carrier marked as the carrier's: a child
entity with a `Collider` on the `Trigger` channel, of any shape, and a
`CarrierVolume` component, which has no fields. The trigger follows the carrier
like any trigger under a moving body. A carrier may have several, one per room
or car. A trigger without `CarrierVolume` is only a trigger, so a pickup zone
on a ship stays a pickup zone. A volume needs no floor: a rider falling through
one is carried while it is inside.

While a rider is kept in the air, by a volume or by the grace time, it stays
in the carrier's frame: it is carried, and it steers and falls relative to the
carrier. A hop on a turning deck lands where it was aimed, and a deck that
speeds up takes the rider with it. Keep the grace time short — long enough for
a hop.

**Grip.** A rider boarding a fast carrier is moving much slower than the deck,
and without help it slides across it until friction stops it, often off the
far side. `grip` decides how much of the carrier's speed it takes up when it
boards, measured against the ground it last stood on. At 1 a rider dropped onto
a train lands moving with it, and one walking on from a platform keeps walking
at the same pace across the deck. At 0 it keeps its speed and slides. Only the
moment of boarding changes; walking and stopping work as always.

| Carrier | `join` | `hold` | `graceTime` |
|---|---|---|---|
| A lift, a flatcar | `Touch` | `Touch` | 0 |
| A boat you can hop about on | `Touch` | `Touch` | 0.5 |
| A spaceship: everyone inside moves with it | `Volume` | `Volume` | 0 |
| A train car boarded by landing on its floor, kept while inside | `Touch` | `Volume` | 0 |

**Choosing the base from gameplay.** Set `CharacterIntent.base` to a body, and
the character rides it whatever it touches, until the field is set back to
`NullEntity`. Like `stance`, it is held, not a one-step request. The body needs
a `RigidBody` but no `Carrier`, and no carrier rule or `RidesBases` applies to
it, so a cutscene can seat a player in a cart nobody else should ride. If the
body is destroyed, the engine lets the character go and clears the field. A
game that wants its own rule for which carrier a character rides, say by team,
writes this field.

```cpp
scene.GetMut<Physics::CharacterIntent>(player)->base = cart;            // seat
scene.GetMut<Physics::CharacterIntent>(player)->base = ECS::NullEntity; // let go
```

Riding needs no `Parent`, and the character keeps none. Only characters ride;
a crate on a deck stays on it by friction.

The base moves the character without sweeping it through the level. Walls
that are part of the carrier move with it and are never a problem. A wall that
is not part of the carrier is something the character's own sweep runs into
and stops at, as it would if it were walking.

Two `options` on the character decide whether it rides at all:

| Option | Cleared |
|---|---|
| `RidesBases` | The character never rides. On a carrier it takes the carrier's velocity like on any other platform. |
| `TurnsWithBase` | The character rides, but its facing stays where gameplay left it. |

```cpp
// A turret gunner who rides the boat but aims by itself.
character.options = Physics::AllCharacterOptions.Without(Physics::CharacterOption::TurnsWithBase);
```

## Crouching and the crouch jump

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

## When two characters meet

Two characters block each other: one walking into another stops against it,
and the other doesn't move. It's the same in a networked game, where every
machine agrees on it, so bumping into another player never causes a
correction. Bodies are different: a crate or a ball moving into a character
still pushes it, unless you turn its `Pushable` option off.

To let characters walk through each other instead, for example your players,
give them a collision channel of your own and leave it out of their
`collidesWith`. A character meets another only when each one's channel is in
the other's `collidesWith`, so players on that channel pass through each other
but still meet everything else, other characters included.

## `Character` settings

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
| `options` | all | What the character does: `PushesBodies` and `Pushable` turn either direction of pushing bodies off; `RidesBases` and `TurnsWithBase` are described under riding a moving body. |
| `collidesWith` | all | Which collision channels the character hits. |

Changing a setting of any of these components in the editor applies at once,
and the character keeps its place and speed.
