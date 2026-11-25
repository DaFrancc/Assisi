# Blending and smoothing values

Games often move one number toward another: a health bar draining, a camera
following a player, a marker gliding to a new spot. `Assisi/Math/Interpolate.hpp`
has the common ways to do it, in the namespace `Assisi::Math`. Each takes a
`float` or a `double`, and most also take vectors such as `glm::vec3` and
colours.

```cpp
#include <Assisi/Math/Interpolate.hpp>
```

## Blending between two values

| Function | What it gives |
|---|---|
| `Lerp(a, b, t)` | The point `t` of the way from `a` to `b`: `a` at `0`, `b` at `1`, halfway at `0.5`. |
| `InverseLerp(a, b, value)` | The opposite: how far `value` is from `a` toward `b`, as a number from `0` to `1`. |
| `Remap(value, inA, inB, outA, outB)` | `value` moved from one range onto another. `Remap(health, 0, 100, 0, 400)` turns health into a bar's width. |
| `Clamp(value, low, high)` | `value` kept between `low` and `high`. On a vector, each part is kept separately. |
| `Saturate(value)` | `value` kept between `0` and `1`. |
| `SmoothStep(edge0, edge1, x)` | `0` before `edge0`, `1` after `edge1`, and a soft S-curve between. Good for fades. |
| `SmootherStep(edge0, edge1, x)` | The same, even softer at the ends. |
| `Slerp(a, b, t)` | `Lerp` for rotations (`glm::quat`). It turns at a steady speed, the short way round. |

`Lerp`, `InverseLerp` and `Remap` don't stop at the ends: a `t` of `2` goes past
`b`. Use `Saturate` or `Clamp` to keep the result inside.

## Easing toward a target every frame

These run once per frame and move a value a little closer to where it should
be. Each takes `dt`, the frame's length in seconds (`ctx.dt` in a system), so
the movement looks the same at any frame rate.

| Function | How it moves |
|---|---|
| `Damp(current, target, seconds, dt)` | Fast at first, slowing as it gets close. `seconds` sets the pace: it covers about two thirds of the distance in that time, and nearly all of it in three times that. `0` jumps straight there. |
| `DampAngle(current, target, seconds, dt)` | `Damp` for an angle in radians. From 350 degrees to 10 it goes up through 360, not all the way back down. |
| `DampRotation(current, target, seconds, dt)` | `Damp` for a rotation. |
| `SmoothDamp(current, target, velocity, seconds, dt)` | Like a spring: it speeds up, then slows down before it arrives, and never goes past. `seconds` is roughly how long it takes. It needs a `velocity` you keep between frames, starting at zero. |
| `MoveTowards(current, target, maxStep)` | At a steady speed, stopping exactly on the target. Pass `speed * dt` as `maxStep`. |

For example, a marker that follows where the player aims, smoothly. `Marker` is
a component of your own, with a `glm::vec2 position` that a screen binds:

```cpp
// About a tenth of a second to catch up most of the way.
constexpr float kFollowSeconds = 0.1f;

marker.position = Assisi::Math::Damp(marker.position, aim, kFollowSeconds, ctx.dt);
```

`Damp` is the simplest choice and suits most things. Choose `SmoothDamp` when a
sudden start looks wrong, such as a camera that begins to move. Choose
`MoveTowards` when the speed must stay the same, such as a door that slides
open.

Don't write `current += (target - current) * 0.1f` every frame. It moves faster
at a higher frame rate, so the game feels different on every computer. `Damp`
is that line done right.
