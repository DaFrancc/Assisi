# UI in the world

Some UI belongs to a thing in the world rather than to the window. There are
two kinds, and both are ordinary screen files:

- A **world widget** floats over an entity, facing the player: a nameplate
  over a character, a prompt on a door, a marker on an objective.
- A **screen surface** is drawn onto an entity's mesh, as a picture on it: a
  computer monitor, a cockpit display, a sign. The player can walk up to it
  and use it. See [A screen on a surface](#a-screen-on-a-surface).

## A nameplate

The falling cube in the sandbox's `levels/Test.alvl` wears one. The screen,
`assets/ui/Nameplate.amdn`:

```xml
<screen sort="hud" attach="center end">
  <column style="panel" padding="10 6 10 6" gap="2" align="center center">
    <text text_size="18">Crate</text>
    <text text_size="14">{Transform.position:.1f}</text>
  </column>
</screen>
```

The entity wears a `WorldWidget` component that names the file:

| Field | Value |
|---|---|
| `screen` | `ui/Nameplate.amdn` |
| `offset` | `0 1 0` |

And the level runs the `WorldWidgets` system, here through
`blueprints/BaseGameplay.abp`, which lists it with the level's other systems.
Without the system, nothing shows.

The screen's [context](ui-run-time.md#contexts) is its entity, so its
[bindings](ui-bindings.md) read that entity: `{Transform.position}` is the
cube's own position, and a health bar bound to `{Health.current}` is that
character's health. Every entity wearing the widget gets a screen of its own.

## Where it sits

A world widget's screen is as large as its content, not the whole window.
The point `offset` names, in metres from the entity's position along the
world's axes, lands on screen where the camera sees it. `0 2 0` is two metres
above a character's feet, whichever way the character faces.

`attach` on the `<screen>` says which point of the screen goes on that spot,
as two words, across then down, each `start`, `center` or `end`:

| `attach` | Puts this point of the screen on the spot |
|---|---|
| `center center` (default) | Its middle |
| `center end` | Its bottom middle: a nameplate, which sits above the spot |
| `start start` | Its top-left corner |

`attach` means nothing on a screen laid across the window.

## How it behaves

A widget's settings are on its `WorldWidget`, so each entity can behave its
own way.

| Field | Values | Meaning |
|---|---|---|
| `blocked` | `Ignored` (default), `Hides`, `Shows` | `Ignored` shows it whatever is in the way. `Hides` shows it only while nothing stands between the camera and the spot: a nameplate behind a wall disappears. `Shows` shows it only while something does: a marker seen through walls. |
| `offScreen` | `Hides` (default), `StaysOnEdge` | `Hides` lets it go off the edge with its entity, and hides it while the entity is behind the camera. `StaysOnEdge` keeps it on the edge of the screen nearest the entity, behind the camera too: an objective marker. |
| `fadeNear`, `fadeFar` | metres | It starts to fade at `fadeNear` and is gone at `fadeFar`. A `fadeFar` of 0, the default, never fades it. |
| `scaleDistance` | metres | At this distance it is the size its file gives it. Farther away it shrinks, as a thing that far away would. Nearer, it stays that size. 0, the default, never scales it. |

The sandbox's `levels/Test.alvl` has one of each:

- A glowing beacon behind the player's start wears `ui/Waypoint.amdn` with
  `offScreen` set to `StaysOnEdge`. Turn away from it and its marker waits on
  the edge of the screen nearest it.
- Two small cubes stand behind a wall. One wears `ui/WallHidden.amdn` with
  `blocked` set to `Hides`. The other wears `ui/WallXray.amdn` with `blocked`
  set to `Shows`. From the start, only the second shows. Walk round the wall
  and they swap.

What counts as "in the way" for `blocked` is anything with a
[collider](physics.md), found by a ray from the camera to the spot. The
widget's own entity, anything attached to it, and whatever holds the camera
don't count. A wall with no collider doesn't block, and neither do things
drawn without one, such as grass.

## When it goes

- When the entity is destroyed, its screen goes the same frame.
- When the `WorldWidget` is taken off the entity, its screen goes.
- When the widget is given another file, the old screen goes and the new one
  is made.
- When the level unloads, every screen goes with it.

The screen is never saved with the level or sent to other players: only the
component is. In a multiplayer game the component reaches every player with
its entity, a character the host spawns included, and each machine makes its
own screen from it.

## Tips

- Give a world widget's screen the default `input`, so it never takes the
  keys or the mouse from the game.
- A file that fails to load logs an error once, and the entity shows no
  widget until it is given a file that loads.

## A screen on a surface

The sandbox's `levels/Test.alvl` has an old CRT monitor in front of the player.
Its glass, `meshes/crt_screen.gltf`, bulges towards you the way a CRT's does.
Its entity wears a `ScreenSurface`:

| Field | Value | Meaning |
|---|---|---|
| `screen` | `ui/Monitor.amdn` | The screen file to show. |
| `width`, `height` | `640`, `400` | The picture's size in pixels. |
| `materialSlot` | `0` | Which material slot of the mesh shows it. A primitive mesh has one slot, 0. |

The level runs the `ScreenSurfaces` system, through
`blueprints/BaseGameplay.abp`. Without it, the monitor shows its plain
material.

The system draws the screen into a picture of its own and puts that picture
in the slot's material, as its base colour and as its glow. The rest of the
material stays as it is. So the material decides how the screen looks in the
room:

- With an emissive factor above zero, the screen glows and can be read in the
  dark. The sandbox's `materials/screen.amat` uses `3 3 3`.
- With an emissive factor of zero, the screen is only lit by the room, like a
  printed sign.

As with a world widget, the screen's [context](ui-run-time.md#contexts) is its
entity, so `{Ammo.count}` on a turret's monitor shows that turret's ammunition.

### How big it is drawn

The picture's shorter side is 1080 UI pixels, just as the window's is. A
640 by 400 picture is laid out as if it were a 1728 by 1080 display. Write the
file at that size: `text_size="64"` reads well on it.

A larger `width` and `height` show the same layout more sharply. They don't
fit more on it. Keep the picture's shape the same as the surface's, or the
screen looks stretched: the sandbox's monitor is 1.6 by 1 metres and its
picture is 640 by 400.

The picture covers the slot as the mesh's texture coordinates lay it out. On
`prim://quad`, the picture's top-left corner is the quad's top-left. A curved
mesh, such as the sandbox's CRT or a crystal ball, shows the picture curved, and
the cursor still lands where the player sees each button on the curve.

The sandbox's CRT glass comes from `scripts/make-crt-screen.py`. A CRT's glass
is a slice of a sphere, so each point is pushed back by
`R - sqrt(R² - x² - y²)`, where `R` is the sphere's radius. The middle is
nearest, and the corners curve back 18.5 cm. Its texture coordinates run
evenly across the screen, so the picture covers the glass edge to edge and bends
with it. Change the script's `RADIUS` and run it again for a flatter or rounder
screen.

### Using it

Showing a screen on a surface never takes the keys or the mouse. To work it,
the game calls `UseScreenSurface`, say when the player presses a key while
looking at it:

```cpp
#include <Assisi/App/ScreenSurfaces.hpp>

if (ctx.input->IsKeyPressed(Assisi::Window::Key::E))
{
    Assisi::App::UseScreenSurface(ctx, monitor);
}
```

While it is used:

- The mouse cursor is free, and the player's own controls stop, as they do
  under a menu.
- The cursor is traced from the camera onto the mesh. Where it lands on the
  picture is where it is on the screen, so the player clicks the monitor's
  buttons where they see them.
- The keys go to the screen, so the player can type into its fields.

Back (Escape) puts it down. If a field has the keys, the first Back leaves the
field, and the second puts the screen down. The game can also call
`LeaveScreenSurface(ctx)`.

A menu that takes input, such as the pause menu, takes the keys while it is
open, even from a screen in use. The screen gets them back when the menu
closes. The menu doesn't hide the monitor: screens on surfaces are drawn and
ordered among themselves, never with the screens on the window.

The sandbox's `UseMonitor` system, in `apps/game/src/Monitor.cpp`, is a
complete example. It uses whichever screen surface the camera is looking at
within 3 metres. For that, the monitor needs a [collider](physics.md).

### What it costs

A screen on a surface is drawn again only when it changes: when something it
binds to changes, when code changes one of its nodes, or while it is being
used. An idle monitor is a still picture, and costs no UI work each frame
beyond reading its bindings.

At most 64 screen surfaces can exist at once.

### When it goes

- When the entity is destroyed, its screen and picture go the same frame.
- When the `ScreenSurface` is taken off, they go, and the slot shows its own
  material again.
- When any field of the `ScreenSurface` changes, or the mesh does, they are
  made again.

Like a world widget, only the component is saved and sent to other players.
Each machine draws its own picture.

### Limits

- The picture has no smaller copies for distance (mipmaps), so a monitor seen
  from far away can shimmer. Keep pictures no larger than they need to be.
- The screen's colours are blended as they are on the window, then lit as a
  material. Soft edges can look a little different than the same screen on
  the window.
- The cursor is traced onto the mesh as the mesh loaded. A skinned mesh is
  traced in its rest pose.
