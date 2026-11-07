# Sky and time of day

An outdoor level needs a sky behind everything, a sun that lights the world from
the right direction, and maybe a day that passes. All of these are components on
**one entity**: the one with the `DirectionalLight`. That light is the sun.

| Component | Header | What it adds |
|---|---|---|
| `DirectionalLight` | `<Assisi/Runtime/LightComponents.hpp>` | Sunlight. Required: everything below goes beside it. |
| `Skybox` | `<Assisi/Runtime/SkyComponents.hpp>` | A sky drawn behind everything, coloured by that light, and the light the sky casts on the world. |
| `TimeOfDay` | `<Assisi/Runtime/TimeOfDay.hpp>` | A clock: the hour, the day and the time of year. |
| `Sun` | `<Assisi/Runtime/TimeOfDay.hpp>` | Aims the light from the clock, so the sun rises, crosses the sky and sets. |
| `Moon` | `<Assisi/Runtime/TimeOfDay.hpp>` | A moon that rises and sets, goes through its phases, and lights the world at night. |

[Entities and components](entities-and-components.md#the-sky-the-sun-and-the-moon)
introduces these. This page explains what each does in full. The `Pretty` level
(`assets/levels/Pretty.alvl`) has all five on its `Sun` entity.

## Try it: a day that passes

### Step 1: Make the sun

Open the editor with a level that has no directional light, such as
`levels/ShadowCheck.alvl`, or delete the one your level has.

1. Add an entity and give it a `DirectionalLight`. It needs no `Transform`.
2. Add a `Skybox`. A blue sky appears behind the level.

### Step 2: Put it on a clock

1. Add a `TimeOfDay` and a `Sun` to the same entity. The sun moves to where it
   is at noon.
2. Drag `TimeOfDay`'s `hour` between 5 and 20. The sun rises, crosses the sky
   and sets as you drag, and the sky goes orange around sunrise and sunset.
   This works without pressing Play.
3. Set `hour` to `7` and `dayLengthSeconds` to `60`, so a whole day lasts a
   minute.

### Step 3: Let it run

1. In the **Systems** panel, add the `TimeOfDay` system. It moves the clock
   while the game runs. Without it, the time stays where you set it.
2. Press **F5**. The sun climbs and the shadows swing round as the day passes.

Add a `Moon` to the same entity to have a moon in the sky and moonlight at
night.

## Rules for the sky components

- **A sky needs exactly one `DirectionalLight` in the level.** With none there
  is no sun to light the sky. With two or more, the sky is not drawn and the log
  warns once. Without a sky, the background is the `clearColor` from
  [`game.json`](game-settings.md).
- **The `Skybox`, `Sun` and `Moon` must be on the light's entity.** On any other
  entity they do nothing.
- **A `Moon` needs a `Sun`** beside it. Without one the moon is ignored and the
  log says so.
- **A `Sun` needs a `TimeOfDay`** somewhere in the level. Without one the sun
  stands where it would at noon on day 0, and the log says so. The `TimeOfDay` can be on any entity, but the light's entity is the usual
  place.
- **There is one clock.** With more than one `TimeOfDay`, the first the engine
  finds is used and the log warns.
- A `Sun` works without a `Skybox`: the light then moves with the clock, and no
  sky is drawn.

## `Skybox`

The sky is computed from a model of the air: how thick it is, and how much
light the air and the dust in it scatter. This is what makes it blue overhead
and orange at sunset without you painting either.

The colour and brightness of daylight come from the `DirectionalLight`, not from
the `Skybox`. Make the light blue and the sky, the sun's disk and every lit
surface turn blue with it.

The easiest way to choose a sky is `preset`:

| `preset` | The sky |
|---|---|
| `Clear` | A temperate clear day. The default. |
| `Arctic` | Cold, very clean air over snow: deep blue, almost no haze, and bright light thrown back up from the ground. |
| `Savanna` | Hot and dusty: a warm haze that pales the sky towards the horizon. |
| `Tropical` | Humid: the sky pales towards the sun and the horizon washes out. |
| `Alpine` | Thin air at altitude: a dark blue sky and a hard white sun. |
| `Hazy` | Thick, still air: the sun is a pale disk and the whole sky is nearly grey. |
| `Airless` | No atmosphere: a black sky and a hard white sun, as on the moon. |
| `Custom` | Uses the fields below as you set them. |

While `preset` is anything but `Custom`, the fields below are greyed out and
not used. To start a sky of your own from a preset, note the preset's look,
switch to `Custom`, and change one field at a time.

<details>
<summary>The <code>Custom</code> fields</summary>

| Field | What it changes |
|---|---|
| `airScattering` | How much the air scatters each of red, green and blue. Earth's air scatters blue most, which is why its sky is blue and its sunsets red. Change the balance for an alien sky. |
| `airThickness` | How much air there is. `0` is no atmosphere. |
| `hazeScattering` | How much dust and droplets scatter, per colour. Grey on Earth; colour it for a dusty sky. |
| `hazeForwardness` | How much haze throws light onwards rather than back. Higher values make a brighter glow around a low sun. |
| `skyBounce` | How much light scattered out of the sun's path still reaches the eye after scattering again. Too little turns the horizon green. |
| `groundColor` | The colour of the ground the sky lights from below. It is not a surface you see; it is what the sky looks like below the horizon, and it colours the light surfaces get from below. |
| `nightColor` | The colour the sky has with the sun down. |
| `exposure` | How bright the sky is drawn compared to the rest of the scene. Default 10. |
| `sunSizeDegrees` | The size of the sun's disk, as an angle. The real sun is about 0.27°; the default is 0.5° so the disk doesn't shrink to a flickering pixel. |
| `sunEdgeSoftness` | How soft the disk's edge is, as a fraction of its size. |
| `sunLimbDarkening` | How much darker the disk's rim is than its centre. `0` is a flat disk. |
| `sunDiskIntensity` | How bright the disk is. `0` hides the disk and leaves the glow around it. |
| `sunDiskColor` | A tint on the disk only. White keeps the disk the colour of the light. |

</details>

Two more fields work with every preset:

| Field | What it changes |
|---|---|
| `minimumAmbient` | The least light the sky gives the world, at any hour. A night with no moon is close to black, which is realistic but usually unplayable; raise this to keep the ground visible. It doesn't make the sky itself brighter, and it casts no shadows. `0` (the default) is off. |
| `minimumAmbientColor` | The colour of that light. Default a cool blue. |

## `TimeOfDay`

| Field | Meaning |
|---|---|
| `hour` | The time of day, from 0 to 24. 12 is noon, when the sun is highest. |
| `day` | Days since the level began. The moon's phase counts these. |
| `dayLengthSeconds` | How many seconds of play a whole day lasts. Default 600 (ten minutes). |
| `paused` | Stops the daily clock. |
| `dayOfYear` | The time of year, in days. `0` is the March equinox: the first day of spring in the north, of autumn in the south. |
| `yearLengthDays` | How many days a year lasts. Default 12, so each season lasts three days. |
| `seasonsPaused` | Stops the yearly clock, so the seasons stay where they are while the days still pass. |

The clock moves only while the `TimeOfDay` system runs. It runs on the fixed
step, so a day lasts the same time whatever the frame rate.

### Changing the time from code

`<Assisi/Runtime/TimeOfDay.hpp>` has three functions that jump the clock
instead of running it forwards:

| Function | What it does |
|---|---|
| `Jump(clock, day, hour)` | Goes to that day and hour. |
| `JumpForwardTo(clock, hour)` | Goes to the next time it is `hour`: later today if that hour is still to come, otherwise tomorrow. Sleeping until morning is `JumpForwardTo(clock, 7.0)`. |
| `JumpSeason(clock, dayOfYear)` | Goes to that time of year. |

Use these rather than writing `hour` yourself. They keep `hour` between 0 and
24, carrying past midnight into the next day, and they mark the change as a
jump, which makes the shadows and the sky's reflections redraw for the new sun
at once. Each returns `false` and changes nothing if given a value that is not
a number, such as NaN.

#### Example: sleeping until morning

Pressing **Z** skips to 7:00 the next morning. `apps/game/src/Sleep.hpp`:

```cpp
#pragma once

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

/// Skips the clock to the next morning when Z is pressed.
ASYSTEM(Update, name = "Sleep", activeWorldOnly)
void SleepSystem(Assisi::App::SystemContext &ctx);
```

`apps/game/src/Sleep.cpp`:

```cpp
#include "Sleep.hpp"

#include <Assisi/App/World.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/TimeOfDay.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

void SleepSystem(Assisi::App::SystemContext &ctx)
{
    using namespace Assisi;

    if (ctx.input == nullptr || !ctx.input->IsKeyPressed(Window::Key::Z))
    {
        return;
    }
    constexpr double kMorning = 7.0;
    for (auto [entity, clock] : ctx.world.scene.Query<Mut<Runtime::TimeOfDay>>())
    {
        (void)entity;
        Runtime::JumpForwardTo(clock, kMorning);
    }
}
```

Add `Sleep` to the level's systems, as well as `TimeOfDay`.

## `Sun`

| Field | Meaning |
|---|---|
| `latitudeDegrees` | How far north (positive) or south (negative) of the equator the level is. Near the equator the sun passes nearly overhead; far north it stays low. Default 45. |
| `axialTiltDegrees` | How tilted the planet is, which is what causes seasons: long summer days and short winter ones. Earth's is 23.44. `0` means no seasons: every day is twelve hours long. |
| `bearingDegrees` | Turns the compass, so you can choose which side of your level the sun rises on without moving the level. |

With a `Sun`, the `DirectionalLight`'s own `direction` is ignored.

## `Moon`

| Field | Meaning |
|---|---|
| `intensity`, `color` | How much the moon lights the world, and in what colour. Real moonlight is far too dark to play in, so the default, 0.02, is much brighter than reality. |
| `sizeDegrees` | The moon's size in the sky, as an angle. |
| `diskColor`, `diskIntensity` | A tint and a brightness for the moon's picture in the sky. They don't change the light it gives. |
| `atmosphericTint` | How orange a low moon turns, from 0 (not at all) to 1 (as orange as a low sun would be). Default 0.2. |
| `cycleDays` | Days from one new moon to the next. Default 29.53. |
| `phaseAtEpoch` | The phase on day 0: 0 is a new moon, 0.5 a full moon. The default, 0.25, puts a half moon in the afternoon sky. |
| `inclinationDegrees`, `nodeAtEpochDegrees`, `nodeCycleYears` | The tilt and slow turning of the moon's orbit. These decide when eclipses happen; leave them alone unless you want to change that. |

The sun and the moon share the one `DirectionalLight`. While the sun is up it
lights the world; once it sets, the moon takes over the same light and its
shadow, dimmed by its phase: a new moon gives no light at all. A moon in the
daytime sky is drawn, but lights nothing. When neither is up, the world is lit
only by the sky's own light.

## The light the sky casts

Besides the sun, a surface gets light from the whole sky around it, and from
the ground. This is **ambient light**, also called **indirect light**: light
that reaches a surface by any route other than straight from a light. It is
what keeps the side of a building facing away from the sun from being black.

With a `Skybox`, the sky provides it:

- **Diffuse light.** A surface facing up gets the sky's colour, a surface
  facing down gets `groundColor`, and anything in between gets a mix. It
  follows the time of day: bright and blue at noon, warm at sunset, dark at
  night with the moon's light added.
- **Reflections.** Shiny surfaces reflect the sky. The engine draws the sky into
  a small cube of six pictures, called a **reflection probe**, and smooth
  materials read their reflection from it; rough ones read a blurred version.
  The probe is redrawn when the sun or the moon has moved more than half a
  degree. The player can turn reflections off or lower their resolution; see
  [Sky reflections](rendering-settings.md#sky-reflections).

Ambient light comes from every direction at once, so it casts no shadows. It
does light the inside of a room as brightly as a field under the open sky,
because the engine doesn't know the room has a roof. The player's
[ambient occlusion](rendering-settings.md#ambient-occlusion) setting darkens
corners and creases, which helps.

**Without a `Skybox`**, every surface gets a dim, flat white ambient light, the
same from every direction, and nothing is reflected. An indoor level lit only by
its lamps has this.

## Fog

Real air is never perfectly clear: the further away a hill is, the more of the
air's own light lies between you and it, so it fades towards the colour of the
sky. That fading is what makes distance read as distance. The `Fog` component,
in `<Assisi/Runtime/SkyComponents.hpp>`, gives a level two kinds of it:

- **Haze**: a thin fog over the whole level that builds up with distance. Far
  hills turn blue-grey; near ones stay clear.
- **Mist**: a thicker fog that lies low, filling valleys and canyons while the
  ground above it stays clear.

A level has at most one `Fog`. It can go on any entity; the sun's entity is a
good place for it. Without one, a level has no fog and pays nothing for it.

### Try it: haze and mist

Open the editor on `levels/FogCheck.alvl`: a plateau, a canyon, trees on the far
rim and mountains behind.

1. Select the `Fog` entity. Its haze lets you see 30 km, and a mist fills the
   canyon below a height of −200 m.
2. Drag `mistHeight` up towards `0`. The mist rises out of the canyon until it
   covers the plateau too.
3. Set `hazeVisibility` to `3000`. The mountains, 4 to 5 km away, disappear into
   the haze, and nothing past 3 km is drawn at all.
4. Press **F1** to hide the editor and see the level as the game would.

### `Fog` fields

| Field | Meaning |
|---|---|
| `hazeVisibility` | How far you can see through the haze, in metres, where it is thickest. At this distance the haze hides almost everything behind it. **Nothing past it is drawn.** `0` (the default) is no haze. |
| `hazeHeight` | Below this world height the haze is at its thickest. |
| `hazeFalloff` | How many metres above `hazeHeight` the haze takes to thin to about a third. Mountaintops then stand clearer than the valleys at the same distance. `0` keeps the haze the same at every height. |
| `mistVisibility` | How far you can see through the mist where it is thickest, in metres. `0` (the default) is no mist. Unlike the haze, it never limits how far is drawn. |
| `mistHeight` | Below this world height the mist is at its thickest: set it to the height of the valley floor it fills. |
| `mistFalloff` | How many metres above `mistHeight` the mist takes to thin to about a third. A small value gives the mist a visible top. |
| `color` | The fog's colour in a level with no `Skybox`. With a sky, it is not used. |

### How much fog a pixel gets

Fog depends on **how much of it lies between the camera and what you're looking
at**, not on where that thing is. Standing inside the mist, a rock 2 m away is
still clear, because there are only 2 m of mist in front of it; the same rock
300 m away, seen through 300 m of mist, is hidden. The heights decide how thick
the fog is at each point along the way, and the distance decides how much of it
you look through.

### Its colour

With a `Skybox`, the fog takes the colour of the sky in the direction you are
looking, without the sun's or the moon's disk. It is pale blue at noon, glows
warm towards a low sun, and goes dark at night, all without anything to author.
Looking down, it takes the colour of the horizon in that direction.

Without a `Skybox`, the fog is `color`, and the background where nothing is
drawn is the same colour, so the fog fades into it. A foggy cave or interior
works this way.

### The draw distance

A `Fog` with haze also sets how far the camera draws: the nearer of the camera's
`farZ` and `hazeVisibility`. Whichever is nearer, everything fades fully into
the fog's colour over the last fifth of that distance, so the edge of the world
is never a visible wall.

With fog on, the sky below the horizon is drawn in the horizon's colour rather
than the ground's. Past the draw distance there is nothing left to see but fog,
and that is what fully fogged ground looks like, so the far edge of the terrain
meets it without a seam.

### What it costs

Fog is worked out for every pixel in the lit pass. Its colour is a sky
calculation, done only where enough fog lies in front of a surface to show, so
the near ground pays almost nothing. A player can be allowed to turn fog off;
see [Fog](rendering-settings.md#fog) in the graphics settings.

## Fog volumes

`Fog` covers the whole level. A **fog volume** puts fog in one place only:
mist over one swamp, fog filling a hollow, smoke in a room, steam in a cave.
It has its own colour and thickness, thins out towards its edges, and leaves
everything outside it clear.

There is one component per shape, all in `<Assisi/Runtime/FogVolume.hpp>`:

| Component | Shape |
|---|---|
| `FogBox` | A box of `size` metres, centred on the entity. |
| `FogSphere` | A sphere of `radius` metres, centred on the entity. Scale the entity to stretch it into an ellipsoid. |
| `FogOutline` | A closed curve you draw on the ground, filled from the entity's origin up to `height` metres. The curve is a spline, shaped in the viewport like any other; see [Splines](splines.md). |

Each moves, turns and scales with its entity, and an entity may hold more than
one. A volume works with or without a `Fog` in the level.

### Try it: mist in three shapes

Open the editor on `levels/FogVolumeCheck.alvl`: pillars and trees on a meadow,
with a box of mist on the left, an ellipsoid of mist in the middle and a swamp
on the right.

1. Select **Mist box**. A faint wireframe shows where its box is; the mist
   fades out well inside it, because its edges are soft.
2. Set its `edgeSoftness` to `0`. The mist now ends sharply at the box's faces.
   Set it back to `2`.
3. Drag its `visibility` down to `4`. The feet of the pillars standing in it
   all but disappear.
4. Its `falloff` is `1.5`, so the mist lies thickest on the ground. Set it to
   `0` and the box fills evenly to its top. Set it back to `1.5`.
5. Select **Swamp**. Its outline is drawn on the ground. Drag one of its nodes
   outwards, and the mist follows. Raise `height` to `6`.
6. Select **Smoke**. Its **Lit by sky** is off, so it keeps its dark colour.
   Turn it on: it becomes a dark tint over the sky's light.

### Fog volume fields

Every fog volume has these five:

| Field | Meaning |
|---|---|
| `visibility` | How far you can see through it where it is thickest, in metres. A pillar this far into it is all but hidden. `0` is no fog. |
| `edgeSoftness` | How many metres it takes to thin from full to nothing at its edges, so it has no hard border. `0` gives hard edges. Along an edge or at a corner, where two sides meet, both thin it, so it fades out more there. |
| `falloff` | How many metres above its floor it takes to thin to about a third, so it lies thickest at the bottom, as mist does. `0` (the default) keeps it the same at every height. The floor is the bottom of a box or a sphere, and a `FogOutline`'s entity origin. |
| `color` | Its colour. See below for how **Lit by sky** changes it. |
| `litBySky` | **Lit by sky**, on by default. Turn it off for caves and rooms the sky does not reach. |

And each shape has its own size: `size` for a `FogBox`, `radius` for a
`FogSphere`, and the `outline` curve and its `height` for a `FogOutline`. An
outline always closes, from its last node back to its first; only where its
nodes are across matters, not their height.

### Its colour

With **Lit by sky** on, `color` tints the light the level's haze takes from the
sky in that direction. White matches the haze exactly, and changes with the time
of day as the haze does. In a level with no `Skybox`, it tints the `Fog`'s
`color` instead, or is drawn as it is if the level has no `Fog` either.

A white volume lit by the sky does not show against the sky itself, because the
light it takes from the sky is the sky's own colour. For mist that stands out
against the sky, tint it, or turn **Lit by sky** off.

With **Lit by sky** off, `color` is drawn exactly as set, at every hour.

### How it is drawn

Each pixel works out how much of each volume lies between the camera and what
the pixel shows, less what the soft edges thin away, and fogs it by that much.
Standing inside a volume, things near you are clear and far ones fade; the sky
behind a volume is fogged the same way. Where volumes overlap, their fog adds up
and their colours mix.

Fog volumes are not lit by the sun directly and nothing casts a shadow into
them, so they show no light shafts.

### What fog volumes cost

The screen is cut into squares 64 pixels wide, and each square lists the volumes
that may cover it. A pixel works only on its own square's volumes, so a volume
costs in proportion to how much of the screen it covers, and a level with none
pays nothing. A `FogOutline` costs more per pixel than a box or a sphere, and
more the more its curve bends, because a pixel checks each straight piece of it.

The player's fog setting turns fog volumes off along with the level's `Fog`. One
frame draws at most 256 fog volumes on screen; past that, the rest are left out
and the log says so once.
