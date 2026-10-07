# Joints

A **joint** attaches one body to another body, or to the world, so that they
move together in a particular way: a door turning on its hinges, a lantern
swinging on a chain, a ragdoll's arm bending at the elbow.

A body cannot have a `Parent` (see [Physics](physics.md)), so a joint is the
only way to attach two bodies to each other.

## Adding a joint

A joint is a component on one of the two bodies. That body is the joint's
**owner**; the body it is attached to is its **other** body. The joint holds
both ways: the other body pulls on the owner exactly as the owner pulls on
it, so which of the two carries the component makes no difference to how they
move.

Each kind of joint is its own component:

| Component | What it lets the two bodies do | Use it for |
|---|---|---|
| `FixedJoint` | Nothing. They move as one, as if welded. | Gluing two objects together so that they can break apart later. |
| `PointJoint` | Turn freely, in any direction, about one shared point. | A chain link, a wrecking ball, a ball-and-socket. |
| `HingeJoint` | Turn about one axis. | A door, a wheel, a lever, an elbow or a knee. |
| `SliderJoint` | Slide along one axis, without turning. | A drawer, a piston, a lift on a rail. |
| `DistanceJoint` | Stay within a range of distances of each other. | A rope, or a rigid rod when the range is a single length. |
| `SwingTwistJoint` | Tilt one axis within a cone, and twist about it. | A shoulder, a hip, a neck. |

Every joint has these fields:

| Field | Meaning |
|---|---|
| `other` | The other body. Leave it empty to attach the owner to the world, which holds it in place. |
| `anchor` | The point the joint is at, in the owner's own space: (0, 0, 0) is the owner's origin. |
| `breakForce` | How hard, in newtons, the joint can be pulled before it breaks. 0 never breaks. See [Breaking](#breaking). |
| `collideConnected` | Whether the two bodies collide with each other. Off by default. See below. |

The entity carrying a joint needs a body of its own: a `Collider` and a
`Transform`, with or without a `RigidBody`. The other body is any body. If it
is a child collider that is part of a bigger body, the joint attaches to that
bigger body.

**The two bodies do not have to touch.** When the joint is first built, each
body is held where it is relative to the other. A `FixedJoint` between two
crates a metre apart keeps them a metre apart, like an invisible bar between
them. A `HingeJoint` holds the other body at whatever distance it is from the
hinge's anchor.

**Bodies joined directly do not collide with each other.** A ragdoll's forearm
can sit partly inside its upper arm where the elbow is, and still hit its own
torso, because the forearm and the torso are not joined directly. Tick
`collideConnected` to make the two bodies collide anyway.

A joint is built once both of its bodies exist, so a joint whose other body is
spawned later attaches when that body appears. It stops holding when either
body is destroyed or loses its `Collider`, or when the joint component is
removed. Bodies attached by joints move each other: if the last link of a
chain goes over a cliff, it pulls the rest of the chain after it.

## Limits

Most joints can be limited, so they move only so far:

| Component | Limit fields |
|---|---|
| `HingeJoint` | `minAngle`, `maxAngle`: degrees either way from the angle the joint was built at. -180 to 180 turns freely. |
| `SliderJoint` | `minDistance`, `maxDistance`: metres along the axis from where it was built. |
| `DistanceJoint` | `minDistance`, `maxDistance`: metres between the two ends. Make them equal for a rod. |
| `SwingTwistJoint` | `swingAngle`: degrees the axis may tilt in any direction. `minTwist`, `maxTwist`: degrees of twist about the axis. |

A limit stops the joint dead. To make a hinge, slider or distance limit **soft**
instead, set `limitSpringFrequency` above 0. The joint can then go past the
limit, and a spring pulls it back. A higher frequency is a stiffer spring.
`limitSpringDamping` says how quickly it stops bouncing: 0 keeps bouncing, and
1 settles without bouncing at all.

**Friction** on a `HingeJoint`, `SliderJoint` or `SwingTwistJoint` resists
movement, in newton-metres for turning and in newtons for sliding. A door with
friction stays where it is pushed instead of swinging freely, and friction on a
ragdoll's joints makes it stiff instead of floppy.

## Motors

A **motor** moves a joint by itself. A `HingeMotor` turns a `HingeJoint` on the
same entity, and a `SliderMotor` slides a `SliderJoint`:

| Field | Meaning |
|---|---|
| `mode` | `Velocity`: keep moving at `target` per second. `Position`: move to `target` and hold there. |
| `target` | Degrees per second or degrees for a hinge; metres per second or metres for a slider. |
| `maxTorque`, `maxForce` | The most the motor pushes with. 0 is no limit. A motor with a limit can be stopped by something heavy enough in its way. |

A motor requires its joint: adding a `HingeMotor` to an entity without a
`HingeJoint` adds one.

## Breaking

A joint with a `breakForce`, or a `breakTorque` for twisting, breaks when a
step pulls or twists it harder than that. A broken joint's component, and its
motor's, are removed, the two bodies come apart, and the world reports it for
that one step in `BrokenJoints()`:

```cpp
ASYSTEM(PostFixedUpdate, name = "CreakOnBreak") void CreakOnBreakSystem(SystemContext &ctx)
{
    for (const Physics::JointBroke &broke : ctx.world.physics.BrokenJoints())
    {
        // broke.owner carried the joint; broke.other is what it held, or
        // NullEntity for the world. broke.kind says which joint it was.
    }
}
```

Like `ContactEvents()`, the list covers only the most recent physics step, so
read it from a `PostFixedUpdate` or `FixedUpdate` system.

In a multiplayer game, only the server breaks joints. A client's copy of a
joint breaks when the server's does.

## Try it: a door on a hinge

1. Add an entity named `Door`. Give it a `Collider` with `shape` Box and
   `halfExtents` (0.5, 1, 0.05), and a `RigidBody`.
2. Add a `HingeJoint` to it. Leave `other` empty, so the door is hinged to the
   world. Set `anchor` to (-0.5, 0, 0), the door's left edge, and leave `axis`
   at (0, 1, 0), straight up.
3. Set `minAngle` to -100 and `maxAngle` to 0, so the door opens one way only.
4. Press Play and push the door. It swings on its left edge and stops when it
   is open.
5. Stop, set `friction` to 20, and press Play again. The door now stays where
   it is pushed.

With the collider overlay on, the editor draws each joint: a small cross at its
anchor, a line along its axis, and a line to the body it holds.

For a door that opens by itself, add a `HingeMotor` with `mode` Position and
`target` -90, and change `target` back to 0 from a system to close it.

## Example: a lantern on a chain

Each link holds on to the link above it with a `PointJoint` at the point where
they meet. The top link holds on to the world.

```
Link1    Transform (0, 4, 0),    Collider (Capsule), RigidBody, PointJoint (anchor (0, 0.15, 0))
Link2    Transform (0, 3.7, 0),  Collider (Capsule), RigidBody, PointJoint (anchor (0, 0.15, 0), other Link1)
Link3    Transform (0, 3.4, 0),  Collider (Capsule), RigidBody, PointJoint (anchor (0, 0.15, 0), other Link2)
Lantern  Transform (0, 3.0, 0),  Collider (Box),     RigidBody, PointJoint (anchor (0, 0.25, 0), other Link3)
```

Give the lantern most of the weight with a higher `density`, so the chain hangs
straight under it. Neighbouring links overlap where they meet, which is fine:
joined bodies do not collide. For a lantern on a rope instead of a chain, give
the lantern one `DistanceJoint` to the world, with `otherAnchor` at the hook
and `maxDistance` the length of the rope.

## Example: a ragdoll blueprint

A ragdoll is one body per limb, joined at the joints of the skeleton. A
`SwingTwistJoint` suits a shoulder, a hip or the neck, and a `HingeJoint` an
elbow or a knee. Each limb holds on to the one nearer the middle of the body,
and the pelvis holds on to nothing, so the whole ragdoll falls.

Here is part of one as a blueprint: the pelvis, the torso, the head and one
arm. In a blueprint, `other` names another entity of the same blueprint, just
as `Parent` does.

```json
{
  "version": 2,
  "entities": [
    {
      "name": "Pelvis",
      "components": {
        "Transform": { "position": [0.0, 1.0, 0.0] },
        "Collider": { "shape": 2, "radius": 0.15, "halfHeight": 0.1 },
        "RigidBody": {}
      }
    },
    {
      "name": "Torso",
      "components": {
        "Transform": { "position": [0.0, 1.35, 0.0] },
        "Collider": { "shape": 2, "radius": 0.17, "halfHeight": 0.15 },
        "RigidBody": {},
        "SwingTwistJoint": { "other": "Pelvis", "anchor": [0.0, -0.25, 0.0], "swingAngle": 30.0,
                             "minTwist": -20.0, "maxTwist": 20.0, "friction": 5.0 }
      }
    },
    {
      "name": "Head",
      "components": {
        "Transform": { "position": [0.0, 1.8, 0.0] },
        "Collider": { "shape": 1, "radius": 0.12 },
        "RigidBody": {},
        "SwingTwistJoint": { "other": "Torso", "anchor": [0.0, -0.15, 0.0], "swingAngle": 40.0,
                             "minTwist": -60.0, "maxTwist": 60.0, "friction": 2.0 }
      }
    },
    {
      "name": "UpperArmL",
      "components": {
        "Transform": { "position": [-0.35, 1.5, 0.0] },
        "Collider": { "shape": 2, "radius": 0.06, "halfHeight": 0.1, "offsetRotation": [0.7071, 0.0, 0.0, 0.7071] },
        "RigidBody": {},
        "SwingTwistJoint": { "other": "Torso", "anchor": [0.15, 0.0, 0.0], "axis": [1.0, 0.0, 0.0],
                             "swingAngle": 80.0, "friction": 3.0 }
      }
    },
    {
      "name": "ForearmL",
      "components": {
        "Transform": { "position": [-0.65, 1.5, 0.0] },
        "Collider": { "shape": 2, "radius": 0.05, "halfHeight": 0.1, "offsetRotation": [0.7071, 0.0, 0.0, 0.7071] },
        "RigidBody": {},
        "HingeJoint": { "other": "UpperArmL", "anchor": [0.15, 0.0, 0.0], "axis": [0.0, 0.0, 1.0],
                        "minAngle": 0.0, "maxAngle": 140.0, "friction": 2.0 }
      }
    }
  ]
}
```

The other arm and the legs follow the same pattern: a `SwingTwistJoint` at the
hip, holding on to the pelvis, and a `HingeJoint` at the knee. A field left
out takes its default, as in any blueprint.

Spawn the blueprint, and the ragdoll collapses under gravity. Each limb keeps
inside its joint's limits, and limbs that are not joined directly, such as a
hand and the head, still collide with each other.
