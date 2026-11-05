# Drawing far from the origin

A level can be hundreds of kilometres across, and the renderer draws it as
precisely 300 km from the origin as at the origin. For the engine's own
components you don't have to do anything. This page explains what the engine
does, and the one rule for code that draws things itself.

## Positions are doubles, matrices are floats

A `Transform`'s `position` is a `glm::dvec3`, three `double`s, so it is exact to
well under a millimetre anywhere in the world. See
[Positions are doubles](entities-and-components.md#positions-are-doubles).

A graphics card works in `float`s, which are not precise far from zero: 100 km
from the origin, a `float` can only say where something is to the nearest
8 mm, and a mesh drawn there would shake as the camera moves. The renderer
avoids this in two ways.

### `WorldMatrix` is stored inside a cell

The world is divided into **cells**: cubes 512 m wide, with cell `(0, 0, 0)`
centred on the origin. Every entity with a `Transform` has a `WorldMatrix`,
which the engine fills each frame with where the entity is drawn. It holds two
things:

| Field | Meaning |
|---|---|
| `cell` | Which cell the entity is in, as three whole numbers. |
| `inCell` | A `float` matrix of the entity's position, rotation and scale, measured from the centre of that cell. |

Measured from the cell's centre, the position in `inCell` is never more than
256 m along any axis, which a `float` holds to a fraction of a millimetre.

Two functions in `<Assisi/ECS/WorldMatrix.hpp>` read it:

| Function | Gives |
|---|---|
| `ECS::WorldPositionOf(world)` | Where the entity is drawn, as a `glm::dvec3`. |
| `ECS::MatrixFrom(world, origin)` | The whole matrix as a `float` `glm::mat4`, measured from `origin`. |

### Everything is drawn measured from the camera

Each frame, the renderer measures every object from the camera's exact
position before turning anything into `float`s. Everything near the camera is
then a small number, and small numbers are exact in a `float`, wherever the
camera is in the world. Things far from the camera are measured less precisely,
but they are also small on screen, so it doesn't show.

The order is what matters: subtract first, in `double`s, then convert. Turning
a position 300 km out into a `float` first and subtracting afterwards loses the
millimetres before the subtraction can save them.

Shadow maps are measured from the centre of the camera's cell instead. When the
camera crosses into another cell, every shadow map the engine was keeping (see
[Shadows](rendering-shadows.md)) is drawn again, as after the camera jumps
somewhere new: the sun's shadow on the next frame, and spot and point light
shadows over the next few.

## Your own drawing code

A `Render` system (see [Your first system](first-system.md)) gets
`ctx.origin`, the camera's position, and a `ctx.view` matrix measured from it.
Measure every matrix you draw with from the same place:

```cpp
for (auto [entity, world, marker] : ctx.scene.Query<ECS::WorldMatrix, Marker>())
{
    DrawMarker(ECS::MatrixFrom(world, ctx.origin), marker);
}
```

The rule: **never convert a world position to `float` before subtracting the
origin from it.** Subtract in `glm::dvec3`, then convert the difference:

```cpp
const glm::dvec3 target = ECS::WorldPositionOf(world);
const glm::vec3 fromCamera = glm::vec3(target - ctx.origin);   // right
// const glm::vec3 wrong = glm::vec3(target) - glm::vec3(ctx.origin);
```

## Depth and the draw distance

The **depth buffer** stores how far from the camera the nearest surface at each
pixel is, so a surface behind another is hidden. Two surfaces close together
far from the camera can round to the same stored depth, and then flicker through
each other as the camera moves. This is called **z-fighting**.

The engine stores depth as a 32-bit `float` in **reversed Z**: the near plane is
stored as 1 and the far plane as 0. A `float` has the most precision near 0,
which here is where distant surfaces are, so two surfaces a ten-thousandth of
their distance apart stay in the right order from 0.1 m to 20 km. A wall 1 m in
front of another at 10 km does not flicker.

There is still a far plane: the camera's `farZ`, its draw distance. Nothing
past it is drawn. Set it to how far the player should see; see
[Choosing `farZ`](rendering-camera.md#choosing-farz).

`assets/levels/DepthRange.alvl` puts an orange panel just in front of a
checkered one at distances from 1 m to 20 km, for checking that nothing fights
on your machine.
