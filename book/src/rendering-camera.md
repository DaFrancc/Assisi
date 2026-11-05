# The camera

What the player sees is drawn from a **camera**: an entity with a `Transform`,
which says where it stands and which way it faces, and a `Camera` component,
which says how wide it sees and how far. A camera looks along its entity's
local −Z axis.

## `Camera` fields

`Camera` is in `<Assisi/Runtime/Components.hpp>`:

| Field | Meaning |
|---|---|
| `fovDegrees` | The **field of view**: how much of the scene fits on screen from top to bottom, as an angle. A smaller value zooms in. Default 60. |
| `nearZ` | The **near plane**: anything closer to the camera than this, in metres, is not drawn. Default 0.1. |
| `farZ` | The **far plane**, or draw distance: anything further than this, in metres, is not drawn. Default 200. |
| `isActive` | Whether this is the camera the game looks through. |

The field of view is vertical. On a wider window the player sees more to the
sides, not less above and below.

### Choosing `farZ`

`farZ` is how far the player can see. Geometry past it is cut off, and the sky
is drawn behind it, so set it to the distance of the furthest thing the player
should see. It also sets two other limits:

- **The sun's shadows** reach no further than `farZ`, or than the player's
  shadow distance setting if that is shorter. See
  [Shadows](rendering-shadows.md#the-suns-shadow-cascades).
- **The light clusters** are spread from `nearZ` to `farZ` (see
  [What lights cost](rendering-lights.md#what-lights-cost)). A far plane much
  further than you need makes each cluster deeper, so each pixel checks more
  point and spot lights.

The depth buffer keeps surfaces in the right order at any distance up to 20 km,
so a large `farZ` doesn't make distant surfaces flicker through each other. See
[Drawing far from the origin](rendering-far.md).

`nearZ` cuts off anything closer than it. A first-person camera inside a
character's head wants it small enough that the character's own hands aren't
cut off; 0.1 m (the default) suits most games.

## Which camera draws

**In the game**, the view comes from the first `Camera` with `isActive` set
that also has a `Transform`.

- If two cameras are active, the log warns, and one of them is used. Which one
  depends on the order the entities happen to be stored in, so clear `isActive`
  on every camera but one.
- If no camera is active, the game draws from the origin, looking along −Z,
  with the default `Camera` settings. That usually means you see nothing you
  expect: give every level an active camera.

**In the editor**:

| While you are | The view comes from |
|---|---|
| Editing | The editor's own camera, which you fly with the right mouse button. The level's cameras are ignored. |
| Playing or paused | The level's active camera, as in the game. If the level has none, the editor's own camera. |

The editor's own camera draws as far as the level's active camera, its
`farZ`, so you edit the level out to the distance the game shows it. A level
with no active camera is drawn to 200 m. The field of view changes with the
scroll wheel; the near plane can't be changed.

**Players.** In a game with [players](players.md), the `PlayerCamera` system
sets `isActive` on the camera of the player who is playing on this machine, and
clears it on everyone else's. You don't set `isActive` yourself for those.

**Multiplayer.** `Camera` is never sent over the network. Each machine chooses
its own camera, so the host can't take over a client's view.

### Switching cameras from code

To cut to another camera, such as for a cut-scene, set `isActive` on the new
one and clear it on every other:

```cpp
void CutTo(Assisi::ECS::Scene &scene, Assisi::ECS::Entity next)
{
    using namespace Assisi;

    for (auto [entity, camera] : scene.Query<Mut<Runtime::Camera>>())
    {
        camera.isActive = entity == next;
    }
}
```

The `PlayerCamera` system sets the local player's camera active again every
frame, so in a level that runs it, the player's camera stays active alongside
the one you chose.

A camera can be the child of another entity through a `Parent`, such as a
camera on a car's bonnet. It follows the car and is drawn from where the car is
that frame.

## Camera paths for benchmarks

A **benchmark** measures how fast a level draws by flying the camera along the
same route every run, so two runs can be compared. The route is made of
**camera paths**: entities with a `CameraLinePath` or a `CameraCirclePath`, in
`<Assisi/Runtime/CameraPath.hpp>`.

Camera paths are only used by the game's `--benchmark` mode. They do nothing
when the level is played normally. How to run a benchmark is covered in
[Measuring a level](rendering-debugging.md#measuring-a-level).

**`CameraLinePath`** flies the camera in a straight line:

| Field | Meaning |
|---|---|
| `from`, `to` | Where the camera starts and ends. |
| `lookAlongPath` | On (the default), the camera faces the way it is travelling. Off, it faces `lookAt`. |
| `lookAt` | The point the camera faces when `lookAlongPath` is off. |
| `seconds` | How long this path takes, relative to the others. |
| `order` | Where this path comes in the route. Lower plays first. |

**`CameraCirclePath`** flies the camera round a horizontal circle, facing a
point:

| Field | Meaning |
|---|---|
| `center` | The centre of the circle. |
| `radius` | The circle's radius. |
| `height` | How far above `center` the circle is, so the camera can look down on what it circles. |
| `startDegrees` | Where on the circle the camera starts: 0 is on the +X side of `center`, 90 on the +Z side. |
| `sweepDegrees` | How far round it goes. 360 is one full circle; a negative value goes the other way. |
| `lookAt` | The point the camera faces the whole time. |
| `seconds`, `order` | As for `CameraLinePath`. |

Things to know:

- **Every position is in world coordinates.** The entity's own `Transform` is
  not used, so a path entity needs no `Transform`, and moving the entity moves
  nothing.
- **The paths play one after another**, by `order`. At the end of one path, the
  camera jumps to the start of the next.
- **`seconds` sets each path's share of the run**, not its real length. A
  benchmark lasts a fixed time (15 s by default) and speeds the whole route up
  or slows it down to fit. A path whose `seconds` is 0 is skipped.
- **The level's active `Camera` still sets the lens.** The route moves the
  camera; its field of view and planes come from the level's active camera.

In the editor, the route is drawn in cyan: each path's course with arrows
showing which way it goes, a box where it starts, a faint line to what it looks
at, and dashed lines joining the paths in the order they play.

`assets/levels/PerfStress.alvl` has a route of three paths to copy from.
