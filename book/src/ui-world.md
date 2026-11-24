# UI in the world

Some UI belongs to a thing in the world rather than to the window: a nameplate
over a character, a prompt on a door, a marker on an objective. This is a
**world widget**. It is an ordinary screen file, shown over an entity and
moved with it every frame.

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
