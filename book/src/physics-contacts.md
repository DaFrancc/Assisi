# Contacts and queries

This page covers two ways to find out what is happening in the physics world:
contact events, which tell you when bodies start or stop touching, and queries,
which you use to ask questions such as "what is in front of the player?".

## Contact events

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

## Checking which bodies are touching

`IsTouching(a, b)` says whether two entities' bodies are touching, and
`Touching(entity, out)` lists everything one is touching. A trigger that opens
a door while a player stands in it can ask this every step instead of keeping
its own list from the events.

A resting pile of objects produces no events and costs nothing to track. If
you want an event every step for every pair that stays in contact, turn on
`Stay` events for that world with `SetStayEventsReported(true)`.

## Ray casts and overlap tests

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
