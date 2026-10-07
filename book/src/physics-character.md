# The character controller

A player or an NPC needs different physics from a box. It should walk up
stairs, stay upright, stop when the player lets go of the keys, and never be
knocked over. A `Character` component does this.

A character's collision is a capsule (a cylinder with rounded ends). Each step,
the engine moves the capsule through the level by the character's own rules:
it slides along walls, climbs steps and stays on slopes. Those rules copy
Half-Life 2's: the character speeds up towards the direction it is asked to go,
and ground friction brings it to a quick stop when it is asked to stop.

## Character components

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

## Driving a character with `CharacterIntent`

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

## The order of a movement step

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

## Air movement and air strafing

In the air there is no friction, and step 5 changes in one way: the wish speed
is cut down to `airWishSpeedCap` before the controller decides how much speed
may be added. The amount added per second still uses the full wish speed.

- Holding one direction adds speed only up to `airWishSpeedCap` in that
  direction, which is slow. A jump mostly keeps the velocity it started with.
- Turning the wish direction away from the current velocity keeps adding
  speed. Holding a strafe key and turning the view curves the jump and makes
  the character faster. This is air strafing.

## Jumping on the landing step (bunny hopping)

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

## Movement settings

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

## Shape settings

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
