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
| `lodBias` | Multiplies the game's [level-of-detail](rendering-lod.md) bias for this camera. Below 1, it switches to simpler meshes sooner. Default 1. |
| `features` | Which features this camera draws. Default `All`. See [Making a camera cheaper](#making-a-camera-cheaper). |
| `isActive` | Whether this is the camera the game looks through. |

The field of view is vertical. On a wider window the player sees more to the
sides, not less above and below.

### Choosing `farZ`

`farZ` is how far the player can see. Geometry past it is cut off, and the sky
is drawn behind it, so set it to the distance of the furthest thing the player
should see.

A level's [`Fog`](rendering-sky.md#fog) can shorten it: the camera then draws
to the nearer of `farZ` and the fog's `hazeVisibility`, and everything fades
into the fog before that edge. A camera in one level can draw 2 km while the
same camera in a hazier level draws 800 m.

The draw distance also sets two other limits:

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
that also has a `Transform`. A camera with a
[`TextureCamera`](#showing-a-camera-on-a-surface) is never chosen.

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

**Players.** In a game with [players](players.md), every player's blueprint
carries a camera, and each machine looks through its own player's. The
first-person template's `PlayerView` system sets `isActive` on that camera and
clears it on everyone else's, so you don't set `isActive` yourself for those.

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

The first-person template's `PlayerView` system sets the local player's camera
active again every frame, so in a level that runs it, the player's camera stays
active alongside the one you chose. Stop it from doing so for the length of
the cut-scene, or change it in your copy of the template.

A camera can be the child of another entity through a `Parent`, such as a
camera on a car's bonnet. It follows the car and is drawn from where the car is
that frame.

### Moving the view every frame

A camera's `Transform` moves with the fixed step, and between steps the camera
is drawn where the blend between them puts it. To move the view every frame
without moving anything the game simulates, for head bob, a screen shake or a
look of your own that follows the mouse between steps, give the camera a
**`CameraDrawPose`** and set it each frame:

| Field | Meaning |
|---|---|
| `rotation` | The rotation in the world the camera is drawn with, in place of the one its `Transform` gives it. |
| `offset` | Added to where the camera stands, in the world, in metres. |

```cpp
// In an Update system: bob the view while keeping the way the camera faces.
using namespace Assisi;

const ECS::WorldMatrix &world = *scene.Get<ECS::WorldMatrix>(camera);
Runtime::CameraDrawPose *pose = scene.GetMut<Runtime::CameraDrawPose>(camera);
if (pose == nullptr)
{
    pose = scene.Add<Runtime::CameraDrawPose>(camera);
}
pose->rotation = glm::quat_cast(glm::mat3(world.inCell));
pose->offset = glm::vec3{0.f, 0.02f * std::sin(bobPhase), 0.f};
```

Only the drawing changes. The `Transform`, what the game simulates and what is
sent over the network stay as they are. Remove the component to draw the camera
as its `Transform` has it again. It is never saved with a level.

The first-person template uses it for its look: its `LookAhead` system draws
your player's camera turned as far as the mouse has moved, every frame, before
the next fixed step uses the turning (see
[Looking ahead of the step](players.md#looking-ahead-of-the-step)).

## Showing a camera on a surface

A camera can draw into a **picture** instead of to the screen, and a mesh in
the level can show that picture. That is how you make a rifle scope's glass, a
security monitor, or a screen showing a room the player can't see.

You need two components, both in `<Assisi/Runtime/TextureCamera.hpp>`:

- **`TextureCamera`** goes on an entity that already has a `Camera` and a
  `Transform`. The camera then draws into its picture.
- **`CameraSurface`** goes on the mesh that shows the picture.

| `TextureCamera` field | Meaning |
|---|---|
| `width`, `height` | The picture's size, in pixels. Default 512 by 512. Match its shape to the surface's, or the picture is stretched. |
| `update` | When it draws: see the table below. Default `WhileSeen`. |
| `on` | Whether a `WhileOn` camera is drawing. Default on. |

| `CameraSurface` field | Meaning |
|---|---|
| `camera` | The entity with the `TextureCamera` whose picture it shows. |
| `materialSlot` | Which material slot of the mesh shows it. A built-in shape like the cube has one slot, 0. |

The camera's own `Camera` still sets its field of view and draw distance. Its
`isActive` is ignored: a texture camera is never the camera the player looks
through.

### When it draws

Drawing a picture costs about as much as drawing the scene again at the
picture's size, so a texture camera draws only when it needs to. On a frame
it doesn't draw, its surfaces keep showing the last picture.

| `update` | Draws |
|---|---|
| `Always` | Every frame. |
| `WhileSeen` | On a frame when one of its surfaces is in view of a camera that draws. A surface behind a wall still counts, as long as it is in front of the camera. |
| `WhenAsked` | Once, the next time the frame is drawn after you ask. Asked from a system's update, that is the same frame. |
| `WhileOn` | Every frame while switched on. |

`WhileSeen` also follows pictures inside pictures: if the player sees a
monitor that shows a second monitor, the camera of the second one draws too,
and draws first, so the first monitor shows its newest picture.

From code, ask a `WhenAsked` camera for a picture, or start and stop a
`WhileOn` one:

```cpp
using namespace Assisi;

// Take one picture, such as a photo the player snaps.
Runtime::RequestTextureCameraPicture(scene, cameraEntity);

// Turn a security monitor on and off. Starting a camera that is already on,
// or stopping one that is off, does nothing.
Runtime::SwitchTextureCamera(scene, cameraEntity, Runtime::TextureCameraSwitch::Start);
Runtime::SwitchTextureCamera(scene, cameraEntity, Runtime::TextureCameraSwitch::Stop);
```

To see how many texture cameras drew in a frame, look at the
`render/texture-cameras` counter in a [profiler
capture](rendering-debugging.md#the-profiler). It reads 0 when nobody can see
any of their surfaces.

### How the picture looks

The surface shows the picture as light, as bright as the scene the camera saw.
The main camera's exposure then applies to it like everything else on screen.
So a scope's glass at dusk is as dark as the dusk around it, and through a
bright window it is as bright as the window. The rest of the slot's material,
its colour and its shine, isn't drawn.

A few things to know:

- **A camera never draws its own surfaces.** A security camera that can see
  its own monitor shows the room with the monitor missing, not a tunnel of
  monitors.
- **Two pictures that show each other** both draw, but one of them shows the
  other's picture from the frame before.
- **Sun shadows come from the main camera.** A texture camera uses the sun's
  shadows the main camera has, so nothing outside them has a sun shadow in its
  picture, such as what is behind the player. Shadows of spot and point lights
  show everywhere.
- **Where nothing is drawn, the picture is black**, such as the sky in a level
  with neither a `Skybox` nor a `Fog`.
- **Changing `width` or `height`** makes the camera draw once at the new size,
  whatever its `update` says, since the new picture holds nothing yet.
- **It isn't a mirror.** The picture is what the camera sees from where it
  stands. It doesn't change as the player walks past, as a reflection would.
- **Each machine draws its own pictures.** Neither component is sent over the
  network, so put both in the level or in a blueprint, where every machine has
  them.

The editor draws texture cameras while you edit, too. The sandbox level,
`levels/Test.alvl`, has a security screen beside the monitor that shows a
camera watching the player.

## Making a camera cheaper

Every camera can leave features out of what it draws. This matters most for
texture cameras: a small security monitor in a corner doesn't need everything
the player's own view has, and every feature it skips is time saved on each
frame it draws.

`Camera`'s `features` field is a set of these. A camera draws the features in
the set and skips the rest:

| Feature | When left out |
|---|---|
| `Shadows` | No sun, lamp or cloud shadows. |
| `AmbientOcclusion` | No darkening in creases and corners. |
| `Sky` | Black behind everything instead of the sky. |
| `Fog` | No level fog and no fog volumes. |
| `Water` | No oceans, lakes or rivers. |
| `SeeThrough` | No see-through surfaces, such as glass. |

Two more fields make a camera cheaper:

- **`farZ`**, its draw distance. Anything further away isn't drawn at all.
- **`lodBias`**, which makes it switch to simpler meshes sooner. 0.5 picks
  each mesh's simpler versions at half the distance it otherwise would.

A camera can only turn a feature off. If the player has turned shadows off in
the graphics settings, no camera draws them, whatever its `features` say.

The player's own camera takes the same fields. A level that never shows water
can leave `Water` out of its camera, but the graphics settings are usually the
better place for that choice.

For example, a cheap security monitor: the camera skips shadows, ambient
occlusion and see-through surfaces, draws 30 m, and uses simpler meshes
sooner. In a level file, its `Camera` reads:

```json
"Camera": {
    "fovDegrees": 70.0,
    "farZ": 30.0,
    "lodBias": 0.5,
    "features": ["Sky", "Fog", "Water"]
}
```

Or from code:

```cpp
using namespace Assisi;

Runtime::Camera &camera = *scene.GetMut<Runtime::Camera>(monitorCamera);
camera.features = camera.features.Without(Runtime::CameraFeature::Shadows)
                      .Without(Runtime::CameraFeature::AmbientOcclusion)
                      .Without(Runtime::CameraFeature::SeeThrough);
```

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
