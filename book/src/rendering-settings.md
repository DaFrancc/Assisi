# Graphics settings

How good the picture looks, and how much it costs, is partly the player's
choice. The player's graphics settings are saved in `options.json`, next to the
game's executable, with their other settings; see
[What the player can change](game-settings.md#what-the-player-can-change).

This page lists every graphics setting in `options.json`, the name it has in the
editor's **F11** window, and what it costs.

## Where the settings live

- **The file is written with every setting in it** the first time the game
  runs, so a player can find and change each one by hand. The keys are written
  in alphabetical order; the order doesn't matter when reading.
- **Values out of range are corrected.** A number outside a setting's range is
  moved to the nearest end of it, and a word the game doesn't recognise, such as
  a misspelt filter name, is replaced by one of the setting's values, usually
  its default.
- **A broken file falls back to the defaults.** If the file isn't valid
  JSON, every setting uses its default, and the file is left as it is so the
  mistake can be fixed. If a value has the wrong type, such as text where a
  number goes, every setting uses its default and the file is written again
  with them.
- **In the editor**, the **F11** window changes the settings while you watch,
  and saves `options.json` on every change.
- **In a game**, the settings are read when the game starts. Your own settings
  screen can change them through `ctx.settings->Options()` and save them with
  `ctx.settings->Save()`, as the [audio volumes](audio.md#volumes-the-player-sets)
  are. Tone mapping, bloom and the frame rate settings take effect at once. Shadows,
  sky reflections, ambient occlusion and fog take effect the next time the game
  starts. Anti-aliasing takes effect the next time the game starts or its window
  changes size.

## Anti-aliasing

**Aliasing** is the jagged, stair-step look of a slanted edge drawn on a grid of
square pixels. **Anti-aliasing** smooths it.

```json
"antiAliasing": {
    "mode": "none",
    "msaaSamples": 4
}
```

| Key | F11 name | Values | Default |
|---|---|---|---|
| `mode` | **AA Mode** | `"none"`, `"msaa"`, `"fxaa"`, `"msaa+fxaa"` | `"none"` |
| `msaaSamples` | **MSAA Samples** | `2`, `4` or `8` | `4` |

- **MSAA**, multisample anti-aliasing, checks several points inside each pixel
  along the edges of triangles and blends them. It smooths the edges of
  geometry cleanly, but not jagged detail inside a texture or a sparkling
  highlight. Its memory and drawing time grow with `msaaSamples`. If the
  graphics card can't do the number asked for, the most it can do is used, and
  the log says so.
- **FXAA**, fast approximate anti-aliasing, looks for edges in the finished
  picture and blurs along them. It is one cheap pass over the screen, and it
  also smooths texture detail and highlights, but it softens the whole picture
  slightly.
- **`"msaa+fxaa"`** runs both, which gives the smoothest result at the cost of
  both.

## Tone mapping

The renderer adds up light with no upper limit: a sunlit wall can be many times
brighter than white. This is called **HDR**, high dynamic range. A screen can
only show colours from black to white, so the last step of every frame, the
**tone map**, turns the light values into screen colours. Bright areas are
compressed rather than cut off, so they keep their detail and colour.

```json
"toneMap": {
    "operator": "agx",
    "exposureStops": 0.0,
    "dither": true
}
```

| Key | F11 name | Values | Default |
|---|---|---|---|
| `operator` | **Tone Map** | `"agx"`, `"aces"`, `"reinhard"` | `"agx"` |
| `exposureStops` | **Brightness** | −8 to 8 | `0` |
| `dither` | **Dither** | `true`, `false` | `true` |

- **`operator`** chooses the curve. **AgX** fades very bright, strongly coloured
  light towards white without changing its colour. **ACES** has more contrast,
  but very bright colours change hue: a bright blue turns purple, a bright red
  orange. **Reinhard** is a simple, flat curve.
- **`exposureStops`** is the player's brightness. It is added on top of the
  exposure the level sets, so it still works in a level with automatic
  exposure. Each stop doubles or halves it: `1` is twice as bright, `-1` half.
- **`dither`** adds a faint, fixed grain before the picture goes to the screen.
  A screen shows 256 levels of brightness per colour, so where light fades
  slowly, such as a dim floor at the edge of a lamp's reach, the picture holds
  one level for a stretch and then steps to the next: soft bands, like contour
  lines. The grain moves each pixel by less than one level, so the stretch
  between two levels becomes a mix of both and the bands disappear. Turn it
  off only to compare.

The tone map runs every frame whatever it is set to, so these settings cost
nothing.

The level sets the rest of its look, its exposure, contrast, saturation and
white balance, with `PostProcessVolume`s. See
[The level's look](rendering-post-processing.md). An `options.json` from before
that change may still hold `contrast` and `saturation` under `toneMap`; they
are ignored.

## Bloom

**Bloom** is the soft glow around light much brighter than its surroundings:
the sun, a lamp, a bright highlight on metal or water. The engine blurs the
picture, wider and wider, and mixes a small share of the blur back in before
the tone map. The blur replaces the same small share of every pixel, so an
evenly lit wall looks the same, while the sun, hundreds of times brighter than
anything around it, spreads a visible halo over its neighbours.

```json
"bloom": {
    "enabled": true
}
```

| Key | F11 name | Values | Default |
|---|---|---|---|
| `enabled` | **Bloom** | `true`, `false` | `true` |

How strong the glow is and how widely it spreads belong to the level's look:
`bloomIntensity` and `bloomScatter` on a `PostProcessVolume`. See
[Bloom](rendering-post-processing.md#bloom). The glow covers the same share of
the screen at any resolution.

Bloom adds a few passes over a half-size copy of the picture and smaller ones.
Off, it runs no passes and holds no memory.

## Shadows

What these settings do is explained in [Shadows](rendering-shadows.md), with a
table of what each quality tier sets. The F11 window's **Low**, **Medium**,
**High** and **Ultra** buttons set several of them at once.

`shadows.pcss` is the contact hardening: `"off"` (the default), `"sun"` or
`"sunAndLocals"`. F11 calls it **Contact Hardening**.

The sun's shadow, under `shadows.sun`:

| Key | F11 name | Values | Default |
|---|---|---|---|
| `enabled` | **Cast Shadows** | `true`, `false` | `true` |
| `cascades` | **Cascades** | 1 to 8 | `4` |
| `resolution` | **Resolution** | 256 to 4096, a power of two | `2048` |
| `format` | **Depth Format** | `"d16"`, `"d32"` | `"d32"` |
| `maxDistance` | **Distance** | 5 to 500 m | `80` |
| `splitLambda` | **Split Lambda** | 0 to 1 | `1` |
| `filter` | **Filter** | `"point"`, `"pcf3x3"`, `"pcf5x5"`, `"vogel"` | `"pcf3x3"` |
| `depthBiasTexels` | **Depth Bias** | 0 to 8 | `0.25` |
| `slopeBias` | **Slope Bias** | 0 to 8 | `2` |
| `normalOffsetTexels` | **Normal Offset** | 0 to 8 | `0.5` |
| `cascadeBlend` | **Cascade Blend** | 0 to 1 | `0.33` |
| `cadence.enabled` | **Keep Still Cascades** | `true`, `false` | `true` |
| `cadence.driftTexels` | **Cascade Drift** | 0 to 4 | `0.5` |

- **`splitLambda`** sets where the cascades split. `1` makes each cascade a fixed
  number of times longer than the one before it, which keeps every boundary the
  same size; `0` makes them all the same length.
- **`cadence.enabled`** keeps a cascade from the previous frame when nothing in
  it has changed. Turning it off redraws every cascade every frame, which only
  costs time.
- **`cadence.driftTexels`** is how far, in texels, the sun may turn a kept
  cascade's shadows before it is redrawn. Raising it redraws less often while
  the sun moves, with shadows that lag a little.

Spot and point light shadows, under `shadows.local`:

| Key | F11 name | Values | Default |
|---|---|---|---|
| `enabled` | **Cast Shadows** | `true`, `false` | `true` |
| `atlasResolution` | **Atlas** | 512 to 8192, a power of two | `4096` |
| `format` | none; every tier sets `"d16"` | `"d16"`, `"d32"` | `"d16"` |
| `faceResolution` | **Face Size** | 128 to 2048, a power of two | `512` |
| `filter` | **Filter** | as for the sun | `"pcf3x3"` |
| `depthBiasTexels` | **Depth Bias** | 0 to 8 | `1.5` |
| `slopeBias` | **Slope Bias** | 0 to 8 | `2` |
| `normalOffsetTexels` | **Normal Offset** | 0 to 8 | `1.5` |
| `sourceRadius` | **Source Radius** | 0 to 1 m | `0.05` |
| `cache.enabled` | **Cache Static Depth** | `true`, `false` | `true` |
| `cache.redrawMovingLightsEveryFrame` | **Redraw Moving Lights Every Frame** | `true`, `false` | `false` |
| `cache.updateBudgetFaces` | **Redraw Budget** | 1 to 512 | `32` |
| `cache.promoteStillSeconds` | **Settle Seconds** | about 0.017 to 4 | `1` |
| `cache.movingLightUpdateDivisor` | **Mover Update Rate** | 1 to 3 | `1` |

- **`faceResolution`** is the largest tile a light gets; lights small on screen
  get smaller ones.
- **`cache.enabled`** keeps each light's shadow of still meshes between frames.
  It doubles the atlas's memory. Off, every shadowed light is redrawn every
  frame.
- **`cache.redrawMovingLightsEveryFrame`**: a light that moves is normally
  redrawn when the game's fixed step moves it. On, it is redrawn every frame,
  so its shadow follows it exactly, at the cost of a redraw per frame for each
  moving light.
- **`cache.updateBudgetFaces`** is how many tiles may be redrawn in one frame.
  Lights over the budget go without a shadow until their turn. The first light
  waiting is always redrawn in full, so a point light, which needs six tiles at
  once, still gets its shadow when the budget is under six.
- **`cache.promoteStillSeconds`** is how long a mesh must stay still before it
  joins the kept shadows again.
- **`cache.movingLightUpdateDivisor`** lets the less important lights redraw
  their moving meshes only every second or third frame. `1` redraws every frame.

Which lights get a shadow, under `shadows.selection`:

| Key | F11 name | Values | Default |
|---|---|---|---|
| `capEnabled` | **Importance Cap** | `true`, `false` | `true` |
| `capSpot` | **Shadowed Spots** | 0 to 256 | `16` |
| `capPoint` | **Shadowed Points** | 0 to 256 | `4` |
| `capHysteresis` | none | 0 to 1 | `0.15` |
| `classHysteresis` | none | 0 to 1 | `0.25` |

- **`capEnabled`** off doesn't remove the limit of what fits in the atlas; it
  only stops ranking the lights, so the ones that miss out are whichever come
  last rather than the least important.
- **`capHysteresis`** is how much more important a light must be to take a
  shadow from one that has it, as a fraction: `0.15` is 15% more. It stops two
  lights of nearly equal rank swapping their shadow every frame.
- **`classHysteresis`** does the same for tile sizes, so a light near the size
  where its tile would change doesn't keep changing it.

## Sky reflections

Shiny surfaces reflect the sky through a **reflection probe**, a small cube of
six pictures of the sky; see
[The light the sky casts](rendering-sky.md#the-light-the-sky-casts). These
settings only matter in a level with a `Skybox`.

```json
"environment": {
    "enabled": true,
    "resolution": 128,
    "samples": 64,
    "rebakeDegrees": 0.5
}
```

| Key | F11 name | Values | Default |
|---|---|---|---|
| `enabled` | **Reflect the Sky** | `true`, `false` | `true` |
| `resolution` | **Probe Size** | 16 to 512, rounded to a power of two | `128` |
| `samples` | **Probe Samples** | 8 to 1024 | `64` |
| `rebakeDegrees` | **Rebake Angle** | 0 to 10 | `0.5` |

- **`enabled`** off removes the sky's reflections; surfaces still get the sky's
  light, and shiny surfaces reflect a plain blend of the sky's colour above and
  the ground's below instead of the sky itself. It saves the probe's memory and
  the work of redrawing it.
- **`resolution`** is the size of each of the six pictures. Larger gives sharper
  reflections on smooth surfaces.
- **`samples`** is how many points of the sky are averaged to make the blurred
  reflections rough surfaces show. More gives smoother blurs and a slower
  redraw.
- **`rebakeDegrees`** is how far, in degrees, the sun or moon must move before
  the probe is redrawn. With a running clock, a larger value redraws less
  often. A still sky is drawn once.

## Ambient occlusion

**Screen-space ambient occlusion**, or **SSAO**, darkens the ambient light in
corners, creases and the gaps between objects, where light from the sky would
have trouble reaching. It works from the depth of the pixels on screen, so it
needs no authoring, and it adds contact and depth to a scene lit mostly by the
sky. It doesn't darken light from the sun or lamps. It is off by default.

```json
"ambientOcclusion": {
    "enabled": false,
    "samples": 12,
    "radius": 0.5,
    "strength": 1.0
}
```

| Key | F11 name | Values | Default |
|---|---|---|---|
| `enabled` | **Screen-Space Occlusion** | `true`, `false` | `false` |
| `samples` | **Occlusion Samples** | 4 to 32 | `12` |
| `radius` | **Occlusion Radius** | 0.05 to 4 m | `0.5` |
| `strength` | **Occlusion Strength** | 0.25 to 4 | `1` |

- **`samples`** is how many nearby points are checked for each pixel. Fewer is
  faster and noisier.
- **`radius`** is how far from a surface, in metres, something counts as
  shading it.
- **`strength`** above 1 darkens more; below 1, less.

On, it adds three passes over the whole screen every frame. Their cost grows
with the screen's resolution and with `samples`.

## Fog

Whether a level's [`Fog`](rendering-sky.md#fog) is drawn. Fog can hide things
a game relies on, such as mist covering a valley, so whether players may turn
it off is your game's choice. It is closed unless you open it: set
`playerSetsFog` to `true` in [`game.json`](game-settings.md#gamejson).

```json
"fog": {
    "enabled": false
}
```

| Key | F11 name | Values | Default |
|---|---|---|---|
| `enabled` | **Fog** | `true`, `false` | `true` |

- **Read only when your game allows it.** With `playerSetsFog` off, fog is
  always drawn, whatever this says. The file only holds it once a player has
  chosen; your own settings screen sets it through `ctx.settings->Options()`.
- **Off draws no fog**, neither the level's `Fog` nor its
  [fog volumes](rendering-sky.md#fog-volumes), and saves their cost. The draw
  distance the level's `Fog` sets still holds, so turning it off changes how
  the edge of the world looks, a hard cut instead of a fade, never what is
  drawn.
- **In the editor**, F11's **Fog** checkbox works whatever `playerSetsFog` says,
  so you can compare a level with and without its fog.

## Clouds

How finely a level's [`Clouds`](rendering-sky.md#clouds) are drawn, and
whether they are. These settings only matter in a level with clouds.

```json
"clouds": {
    "resolution": 2048,
    "steps": 24
}
```

| Key | F11 name | Values | Default |
|---|---|---|---|
| `resolution` | **Cloud Map Size** | 256 to 4096, rounded down to a power of two | `2048` |
| `steps` | **Cloud Steps** | 8 to 256 | `24` |
| `enabled` | **Clouds** | `true`, `false` | `true` |

- **`resolution`** is the width of the map of the sky the clouds are drawn
  into; its height is a quarter of that. Larger gives sharper clouds. It costs
  in proportion to the map's area, whatever the screen's size, and it is the
  setting that changes the clouds most.
- **`steps`** is how many points along each line of sight through the clouds
  are looked at, looking straight up. A line of sight toward the horizon
  crosses more cloud and gets up to three times as many. Fewer is faster, and
  shows as blotches inside the clouds. Above about 24, more steps change
  little.
- **Low**, **Medium**, **High** and **Ultra** in the F11 window set both. Each
  tier costs about two to three times the one below; measured on an RTX 3070:

  | Tier | `resolution` | `steps` | Time per frame |
  |---|---|---|---|
  | Low | 512 | 16 | 0.18 ms |
  | Medium | 1024 | 24 | 0.32 ms |
  | High | 2048 | 24 | 0.82 ms |
  | Ultra | 4096 | 24 | 2.7 ms |

- **`enabled` is read only when your game allows it**: set `playerSetsClouds`
  to `true` in [`game.json`](game-settings.md#gamejson). The file only holds it
  once a player has chosen. Off draws no clouds and no cloud shadows, and saves
  their cost. The sky's light still greys under the clouds as it would, so
  turning them off changes the sky, never how the level is lit.
- **In the editor**, F11's **Clouds** checkbox works whatever
  `playerSetsClouds` says.

## Frame rate

```json
"frameSync": {
    "mode": "vsync",
    "fpsLimit": -1,
    "snap": "game"
}
```

| Key | F11 name | Values | Default |
|---|---|---|---|
| `mode` | **VSync** / **FPS Limit** | `"vsync"`, `"fpsLimit"` | `"vsync"` |
| `fpsLimit` | **Unlimited**, **Max FPS** | `-1` for unlimited, or 1 to 32767 | `-1` |
| `snap` | **Frame Snap** | `"game"`, `"snapped"`, `"unsnapped"` | `"game"` |

- **`"vsync"`** draws one frame per refresh of the display, and `fpsLimit` is
  ignored.
- **`"fpsLimit"`** draws as fast as `fpsLimit` allows, or as fast as it can with
  `-1`. A frame can then be shown partway through the display's refresh, which
  shows as a horizontal tear.
- **`snap`** is explained in [Game settings](game-settings.md).

## What to turn down first

When a game runs too slowly, these save the most, roughly in order. Measure in a
**ship** build ([Build types](build-types.md)); a debug build's costs are
misleading.

1. **Shadows.** Lower the tier, or reduce **Cascades** and **Distance**. Turn off
   **Contact Hardening**. See [Making shadows cheaper](rendering-shadows.md#making-shadows-cheaper).
2. **MSAA samples.** 8 to 4, or 4 to 2, or FXAA instead.
3. **Ambient occlusion**, or its **Occlusion Samples**.
4. **The resolution**: fewer pixels makes every per-pixel cost smaller.
5. **Sky reflections' Probe Samples**, if the sun moves quickly.
6. **Clouds**: a lower tier, or fewer **Cloud Steps**.

The [frame page](rendering-frame.md) shows which pass each setting changes.
