# Physics

Physics is what makes objects in a level solid: a box falls under gravity,
lands on the floor instead of passing through it, and is knocked over when
something hits it. Assisi uses the [Jolt Physics](https://github.com/jrouwe/JoltPhysics)
library for this, but you never call Jolt yourself. You add physics components
to entities, and the engine creates and updates the simulated objects for you.

This page shows how to make objects solid and moving. The pages after it cover
the rest:

| Section | What it covers |
|---|---|
| [Colliders on child entities](physics-child-colliders.md) | One moving object built from several shapes, and shapes that follow a character. |
| [Collision from a model](physics-model-collision.md) | Using a model's own shape for collision instead of a box or a sphere. |
| [Collision channels](physics-channels.md) | Choosing which objects collide with which. |
| [How the scene and physics stay in step](physics-scene-sync.md) | When the engine updates physics from your entities, and what moving an entity's `Transform` does. |
| [Moving bodies from code](physics-moving-bodies.md) | Pushing, launching and placing objects from a system. |
| [Joints](physics-joints.md) | Attaching objects to each other: doors, chains, ragdolls. |
| [Contacts and queries](physics-contacts.md) | Finding out what hit what, and casting rays to find objects. |
| [The character controller](physics-character.md) | Players and NPCs that walk, jump and crouch. |

## Words used in this chapter

| Word | Meaning |
|---|---|
| **Body** | The object the physics engine simulates for an entity. The engine makes one for every entity with a `Collider`. |
| **Static** | A body that never moves on its own: floors, walls, buildings. |
| **Dynamic** | A body that falls under gravity, and is pushed and spun when things hit it. |
| **Kinematic** | A body that moves only where your code puts it, such as a lift or a door. It pushes dynamic bodies out of its way, but nothing pushes it. |
| **Step** | One update of the simulation. Physics runs at a fixed rate, separately from the frame rate. |

## Physics components

All four are in `<Assisi/Physics/PhysicsComponents.hpp>`:

| Component | Add it to | What it does |
|---|---|---|
| `Collider` | Anything that should be solid. | Gives the entity a collision shape, such as a box or a sphere. With only a `Collider`, the entity is static. |
| `RigidBody` | An entity with a `Collider` that should move. | Makes the body dynamic or kinematic, and sets how heavy it is and how it moves. |
| `BodyState` | Nothing: the engine adds it with `RigidBody`. | Holds the body's current speed and spin, updated after every step. |
| `Character` | A player or an NPC. | Makes the entity walk, jump and crouch instead of tumbling like a box. See [The character controller](physics-character.md). |
| `Carrier` | A moving body characters should ride: a boat, a train, a lift. | Moves the characters on it in its own frame. See [Riding a moving body](physics-character.md#riding-a-moving-body). |

## Try it: a floor and a falling box

In the editor:

1. Make the floor: add an entity with a `Transform` and a `Collider`. Set the
   collider's `halfExtents` to `(10, 0.5, 10)`, a slab 20 m wide and 1 m thick.
   It has no `RigidBody`, so it stays where it is.
2. Make the box: add another entity with a `Transform` a few metres above the
   floor, a `MeshRenderer` so you can see it, a `Collider` and a `RigidBody`.
   The default collider is a 1 m cube, and the default `RigidBody` is dynamic.
3. Press **Play**. The box falls, lands on the floor and stops.

Most of a level is like the floor: a `Collider` and nothing else.

## `Collider` fields

The shape:

| Field | Meaning |
|---|---|
| `shape` | `Box`, `Sphere`, `Capsule` (a cylinder with rounded ends) or `Cylinder`. `Convex` and `Mesh` take the shape from a model; see [Collision from a model](physics-model-collision.md). |
| `halfExtents` | For a box: half its size along each axis, in metres. `(0.5, 0.5, 0.5)` is a 1 m cube. |
| `radius` | For a sphere, a capsule or a cylinder, in metres. |
| `halfHeight` | For a capsule or a cylinder: half the length of its straight middle, in metres. |
| `offsetPosition`, `offsetRotation` | Move and turn the shape away from the entity's position, for a shape that should not be centred on it. |

The inspector only shows the size fields the chosen shape uses.

How the surface behaves:

| Field | Meaning |
|---|---|
| `friction` | How much the surface resists things sliding on it. 0 is like ice; 1 grips firmly. Default 0.2. |
| `restitution` | How bouncy the surface is. 0 does not bounce; 1 bounces back at full speed; above 1 speeds up on every bounce, like a pinball bumper. When two colliders touch, the bouncier one decides. Default 0. |
| `density` | How heavy the material is, in kilograms per cubic metre. The body's weight is its volume times this. Default 1000, the density of water. |

The rest, each explained on its own page:

| Field | Meaning |
|---|---|
| `channel`, `collidesWith` | What kind of object this is, and which kinds it collides with. See [Collision channels](physics-channels.md). |
| `attach` | For a collider on a child entity. See [Colliders on child entities](physics-child-colliders.md). |
| `enabled` | Untick it to switch the collider off without removing it: nothing collides with it until it is ticked again. |
| `collisionAsset`, `collisionPiece` | For a shape taken from a model. See [Collision from a model](physics-model-collision.md). |

### Scaling a collider

The entity's `Transform.scale`, including any scale from its parents, scales
the collider too. A box can be stretched on any axis. A sphere or a capsule
has one radius, so it must be scaled the same on every axis, and a cylinder the
same on its two round axes. If you scale one of those unevenly, the engine logs
a warning and uses the closest scale the shape can take.

## `RigidBody` fields

| Field | Meaning |
|---|---|
| `motion` | `Dynamic` (the default) or `Kinematic`. See [Words used in this chapter](#words-used-in-this-chapter). |
| `mass` | The body's weight in kilograms. Leave it at 0 to work it out from the colliders' sizes and `density`. Any other value replaces that total. |
| `linearDamping`, `angularDamping` | How quickly the body slows down and stops spinning on its own, as a fraction lost per second. |
| `gravityScale` | How strongly gravity pulls this body: 1 is normal, 0 floats, 2 is twice as heavy. |
| `lockedAxes` | Directions the body may not move along or turn around. Locking all three rotations keeps a body upright. |
| `ccd` | Turn this on for small, fast objects such as bullets, so they cannot pass through a thin wall between two steps. It costs a little more. |
| `allowSleep` | Whether the body may sleep. A body that has stopped moving is put to sleep: the engine stops simulating it until something touches or pushes it. Leave this on unless the body must keep reacting while still. |

To stop a moving object for a while, such as one the player picks up, switch
its `motion` to `Kinematic`, and back to `Dynamic` to let it go. Its mass and
other settings are kept.

## Trigger volumes

A **trigger** is a collider that notices objects entering and leaving it, but
does not stop them. Use one for a zone that opens a door, a checkpoint, or a
pickup.

To make one, set a `Collider`'s `channel` to `Trigger`. Then react to what
enters it with contact events; see [Contacts and queries](physics-contacts.md).

A trigger with no `RigidBody` notices objects that move into it. If you move the
trigger itself by writing its `Transform`, objects lying still where it lands
are not reported until something wakes them. For a trigger you move around,
add a `Kinematic` `RigidBody` with `allowSleep` off: it then notices objects
lying still too, at the cost of being simulated every step.
