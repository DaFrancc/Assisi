# Colliders on child entities

One `Collider` gives an entity one shape. For an object that is not a single
box or sphere, such as a table with a top and four legs, put more `Collider`s
on **child entities**: entities with a `Parent` pointing at the object.

## What a child collider becomes

The engine looks up the hierarchy from the child to the first entity with a
`RigidBody` or a `Character`. That entity is the child's **owner**, and decides
what the child's collider becomes:

| The child is | It becomes | What that means |
|---|---|---|
| Under a `RigidBody` | A **piece** of the owner's body. | The owner's body is made of all its pieces together. Each piece turns with the body and adds its weight to it. |
| Under a `Character`, or set to ride along (see below) | A **follower**. | A separate body that the engine moves to the child's position after every step. It adds no weight, and never collides with its own owner. |
| Under nothing that moves | **Static**. | A static body of its own, like any `Collider` without a `RigidBody`. |

A child under a `RigidBody` is a follower instead of a piece in two cases:

- Its `attach` field is set to `Body` instead of `Piece`.
- It is on the `Trigger` channel. Jolt makes a whole body a trigger or none of
  it, so a trigger cannot be one piece of a solid body.

The inspector shows which one a collider is under its fields: "Piece of Crate",
"Follows Player", or "Static (no RigidBody)".

The owner itself cannot have a `Parent`: physics decides where a moving body
is, so it cannot also be placed relative to another entity. The editor refuses
to add one. Anything else can still be parented *to* a moving body, such as a
camera or a mesh, and follows it as usual.

## Example: one body made of several shapes

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

## Example: a collider that follows a character

```
Player    Transform, Character
└ Head    Transform (0, 1.6, 0), Collider (Sphere 0.15, channel Hitbox)
```

The head is a follower: it goes wherever the player goes and does not get in
the player's way, because a character's own movement ignores its own
followers. Something falling onto the head is stopped by it, or not, by the
head's `collidesWith`.

## Example: hitboxes that only shots hit

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

## Example: a trigger that follows a moving body

```
Ball      Transform, RigidBody, Collider (Sphere 0.5)
└ Pickup  Transform, Collider (Sphere 1.5, channel Trigger)
```

The pickup radius follows the ball and reports what enters it as contact
events for the ball, with the pickup as the event's `piece`. The ball stays
solid: only the pickup's own body is a trigger. The pickup never reports the
ball itself.

## How hits and calls treat pieces and followers

Pieces and followers belong to their owner, and the engine reports and acts on
them that way:

- **Hits and contacts.** A ray that hits a piece or a follower reports the
  owner as the hit's `entity`, and the child that was hit as its `piece`.
  Contact events do the same.
- **Calls.** Passing a piece or a follower to `Mass`, `BodyOf`, `GetBodyPose`,
  `GetBodyState`, a force or impulse, `IsTouching`, `Touching`, or a query's
  `ignore` acts on the owner. `HasBody` returns false for it, because the body
  belongs to the owner.
- **Followers and their owner.** A follower never collides with or reports its
  own owner, or the owner's other followers.

## Excluding one pair of bodies from colliding

Channels decide which kinds of object collide (see
[Collision channels](physics-channels.md)). To stop just one pair, such as a
projectile and the character that fired it, call `IgnoreCollision(a, b)`.
`IgnoreCollision(a, b, false)` turns collision back on. Passing a piece or a
follower counts as passing its owner, so the exception covers every part of
both. It lasts until you turn it back on or either entity is destroyed.
