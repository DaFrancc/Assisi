# How the scene and physics stay in step

You never create, move or destroy physics bodies directly. You change entities
and their components, and at the start of every step the engine updates the
bodies to match what changed since the last step:

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

## What writing a `Transform` does

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
