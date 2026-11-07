# Looking at rendering problems

When something on screen looks wrong, or a level runs slowly, the editor can
show you what the renderer is doing. This page covers the editor's views and
readouts, a list of common problems, and the tools for measuring how fast a
level draws.

Everything on this page is in the editor, `Assisi-GameEditor`, except the
benchmark, which is in the game. The game has none of the editor's views.

## The F11 window

Press **F11** in the editor to open the **Options** window. Besides the graphics
settings (see [Graphics settings](rendering-settings.md)), it shows what the
renderer is doing.

At the top:

| Shown | Meaning |
|---|---|
| **CPU**, **GPU**, **Frame** | How long the last frame took on the processor and on the graphics card, in milliseconds, and the frame rate. |
| **Frame Time (ms)** | A graph of the last 360 frames, the processor's in orange and the graphics card's in green. A spike is one slow frame. |
| **Avg**, **1% low**, **Min/Max** | The average frame time over that graph, the slowest 1% of frames, and the fastest and slowest. |
| **Clock**, **Util**, **Power**, **VRAM** | The graphics card's speed, how busy it is, its power use and temperature, and its memory. Only on NVIDIA cards. |

A frame takes as long as the slower of the two. If **GPU** is much larger than
**CPU**, the graphics card is the limit, and lowering graphics settings helps.
If **CPU** is larger, the processor is the limit: your systems, physics, or the
number of entities to prepare for drawing. Lowering graphics settings helps
much less there.

The **Rendering** section below them:

| Control | What it does |
|---|---|
| **Frustum Culling** | Untick to draw every mesh, including those out of view, to see what culling saves. |
| **Sort Draws** | Untick to draw meshes in no particular order, to see what grouping copies together saves. |
| **GPU Cull** | Has the graphics card do the culling and pick detail levels, instead of the processor. Off by default. |
| **Editor Overlays** | Untick to hide the editor's outlines, icons and wire shapes. |
| **Skeletons** | Untick to hide the skeletons drawn over animated meshes. |
| **Mesh LOD**, **LOD Bias**, **Force LOD**, **Shadow LOD** | See [Detail levels](rendering-lod.md#looking-at-the-levels-in-the-editor). |
| **Debug View** | Shows one property of the materials instead of the lit picture. See below. |

Under them, **Items** says how many meshes were drawn and how many culled, and
**Draws** how many groups they were drawn in.

None of the controls in the **Rendering** section is saved: they are for looking
at the scene, and reset when the editor restarts.

## Debug views

### Material views

**Debug View** replaces the lit picture with one property of each surface's
material, unlit, so you can check a material's maps are doing what you meant:

| View | Shows |
|---|---|
| **Base Color** | The base colour, without any light. |
| **Metallic** | White where the surface is metal, black where it isn't. |
| **Roughness** | White where it is rough, black where it is polished. |
| **Normal** | Which way each pixel faces, after the normal map, as a colour: red for +X, green for +Y (up), blue for +Z. A flat floor is light green. |
| **Occlusion** | The material's occlusion map: black where it is fully hidden. |
| **Emissive** | The light each surface gives off itself. Black where it gives none. |
| **Screen Occlusion** | What ambient occlusion found: dark in creases. White everywhere while ambient occlusion is off. |
| **Smoothing** | Where a model is smooth-shaded over corners too sharp for its shadows: red, brighter the sharper, on grey. Blue-grey for materials with `ShadowsFollowSmoothing` off. See [Smooth shading and shadows](rendering-smooth-shading.md). |

Bumps that look pushed in where they should stand out usually mean the normal
map was made for a tool that flips its green channel; flip it back in an image
editor. A white **Metallic** on something that isn't metal is a `MetallicFactor`
of 1, the default for a new material, left in place.

### Shadow views

**Shadow View**, in the **Sun Shadows** section, draws over the lit picture to
show what the sun's shadows are doing. It needs a directional light that casts
shadows.

| View | Shows |
|---|---|
| **Cascades** | Tints the picture by which cascade each pixel's shadow comes from: red, green, blue, yellow, magenta, cyan, orange and grey, one for each cascade. Shows where the cascades split, and how wide the blend between them is. |
| **Occluder Margin** | For each pixel, how far in front of it the shadow map's nearest surface is. Red is in shadow, brighter the further the thing casting it; green is lit; black means the two are within a millimetre of each other, where shadow acne can appear; blue is outside every cascade. |
| **Filter Taps** | Compares three answers for each pixel: red is the shadow as drawn, green this cascade's filtered lookup, blue its single centre lookup. Black and white mean they agree. Coloured pixels show where blending or filtering changed the answer, which helps find light leaking into a shadow. |

## Shadow readouts

The **Sun Shadows** section shows how many cascades were drawn and what went
into them. The **Local Light Shadows** section shows:

| Shown | Meaning |
|---|---|
| **Shadowed: N of M lights** | How many shadow-casting spot and point lights have a shadow this frame. |
| **Atlas** | How many lights and tiles the shadow atlas holds, and how full it is. |
| **Dropped by cap**, **atlas full**, **waiting to redraw** | How many lights lost their shadow, and why: the ranking, no room in the atlas, or the redraw budget. |
| **Cache** | How many lights are resting with kept shadows, and how many tiles were redrawn. |

Open **Shadowed Lights** for one row per shadow-casting light: its state, its
tile and its ranking score. The states are the ones the inspector shows for a
selected light; see
[Finding out what happened to a light's shadow](rendering-shadows.md#finding-out-what-happened-to-a-lights-shadow).
**Atlas Tiles**, shown while **Cache Static Depth** is on, lists which light
holds which tile and how long it has been kept.

The **Sky Reflections** section says how big the reflection probe is and how
many times it has been redrawn, or that nothing in the scene is lit by a sky.

## Shapes in the viewport

With **Editor Overlays** on, the editor draws:

- **Lights**: a point light as a sphere the size of its `radius`; a spot light
  as its cone, out to its `radius`, with the inner cone too when selected; a
  directional light as a 2 m arrow pointing the way its light travels.
  Unselected shapes are faint and in the light's own colour.
- **Camera paths**, in cyan. See
  [Camera paths for benchmarks](rendering-camera.md#camera-paths-for-benchmarks).
- **Colliders**, as wire shapes, while you are editing.
- **Skeletons** of animated meshes, while you are editing.
- The orange **outline** of the selection, and **icons** for entities you
  couldn't otherwise see or click, such as lights.

## Seeing the frame without the editor

Two keys help when you are judging how a level looks:

| Key | Action |
|---|---|
| **F1** | Hide the editor: every panel, the gizmo, and all the shapes above. You see the frame as the game draws it. **F1** again brings them back. |
| **F2** | Save a screenshot of the window as it is, panels included if they're showing, to `screenshots/` next to the editor's executable. |

Hide the editor with **F1**, then press **F2**, to keep a picture of the frame
alone, for example to compare a setting on and off. See
[Seeing the level without the editor](editor.md#seeing-the-level-without-the-editor).

## Common problems

| What you see | Likely cause |
|---|---|
| Everything is black. | No light reaches it: a level with no lights and no sky, or a night with no moon. Add a light, or raise the `Skybox`'s `minimumAmbient`. |
| A model is missing. | It is past the camera's `farZ`, still loading, or hidden by `hiddenFromOwnCamera`. The editor's own camera only draws to 200 m. |
| A model is a cube. | Its file failed to load. The log says why. |
| A model is plain white. | Its textures haven't loaded yet, or they are inside a `.glb` file, which isn't read. See [Importing a model](rendering-models.md#importing-a-model). |
| A model looks dark grey and dull. | Its material has a `MetallicFactor` of 1, the default for a new material. Set it to 0 for anything that isn't metal. |
| Two surfaces flicker through each other. | They are in exactly the same place, such as a decal modelled flush with a wall. Move one slightly in front. |
| There is no sky. | The level has no `Skybox` on its directional light, or has more than one `DirectionalLight`. The log warns about the second. |
| A lamp has no shadow. | Select it and read the **Shadow** line in the inspector. |
| Stripes or speckles of shadow on lit surfaces. | Shadow acne. See [When shadows look wrong](rendering-shadows.md#when-shadows-look-wrong). |
| Soft bands, like contour lines, where light fades slowly across a dim surface. | **Dither** is off in the tone map settings. The screen has 256 levels of brightness per colour, and without the dither a slow fade steps from one level to the next. See [Tone mapping](rendering-settings.md#tone-mapping). Turning off the light's `castsShadows` leaves these bands where they are, which tells them apart from shadow acne. |
| Edges are jagged. | Anti-aliasing is off; it is off by default. See [Anti-aliasing](rendering-settings.md#anti-aliasing). |
| Shadows appear a few frames after the camera jumps. | Spot and point light shadows are drawn a few at a time. See [Spot and point light shadows](rendering-shadows.md#spot-and-point-light-shadows). |

## Checking the renderer automatically

`./assisi test --gpu` runs the **GPU tests**. They open every level in
`assets/levels` in the editor, one at a time, each in a small window for a
second or two, and check two things about each:

- **The Vulkan validation layer found nothing wrong.** The validation layer is
  part of the Vulkan SDK. It checks every request the renderer makes of the
  graphics card, including whether two pieces of work that use the same memory
  wait for each other. A mistake there can look fine on your machine and break
  on someone else's, so any error the layer reports fails the test. Install the
  layer first: it comes with the Vulkan SDK, or as a package such as
  `vulkan-validation-layers`.
- **The picture is the one you last accepted.** Each test takes a picture of the
  level and compares it with the one kept for your graphics card in
  `out/golden/<your GPU>/`. If more than a few pixels are visibly different, the
  test fails. It then saves a copy of the expected picture with the different
  pixels in red, as `<level>.diff.png` in `out/build/<build>/apps/game/gpu/`,
  next to the new picture.

One more test, `Gpu-BasePasses`, opens the empty level `PerfBlank` with every
feature switched off and checks that the frame runs only the
[passes](rendering-frame.md) it can't do without. A feature that still runs
when it is switched off fails it.

The first run on a machine has nothing to compare with, so it keeps the
pictures it takes. When you change how things look on purpose, look at the
failing tests' pictures, then accept the new ones:

```bash
./assisi test --gpu --accept
```

The pictures stay on your machine and never go into git: they change every time
the look changes, and every graphics card draws them slightly differently. A
plain `./assisi test` leaves the GPU tests out, because they open windows. To
run one level's test, name it: `./assisi test --gpu -- -R BloomCheck`.

## Measuring a level

Always measure in a **ship** build. A debug build is slow in different places
than a ship build, so its numbers mislead; see
[Build types](build-types.md#never-judge-performance-in-debug).

### The profiler

A build with the profiler, Chiara, compiled in records how long each part of
every frame takes. Build and start one with:

```bash
./assisi run editor ship --profiler
```

Press **F9** to open the **Performance capture** window. Tick **GPU per-pass
timing** to see how long each [pass](rendering-frame.md) takes on the
graphics card. **Dump 5 s** and the other dump buttons save the last few
seconds as a file, and **Start session** records from now until **Stop
session**. Captures are saved in a `captures` folder next to the executable;
open them in [Perfetto](https://ui.perfetto.dev).

Per-pass timing splits the frame into separately timed pieces, which slows it
down a little. Use it to find which pass is slow, not to measure the whole
frame.

### Benchmarking a level along a camera route

A **benchmark** flies the camera along the level's
[camera paths](rendering-camera.md#camera-paths-for-benchmarks) for a fixed
time, so you can compare two runs of the same level, such as before and after a
change. It runs in the game, `Assisi-Game`, and only in a build with the
profiler; in any other build the game doesn't know the `--benchmark` options.

1. Give the level camera paths: entities with a `CameraLinePath` or a
   `CameraCirclePath`. `levels/PerfStress.alvl` has some to copy.
2. Package the game with the profiler:

   ```bash
   ./assisi package ship --profiler
   ```

3. Run the benchmark from the game's folder, naming the level:

   ```bash
   cd out/build/gcc-ship-chiara/apps/game
   ./Assisi-Game --benchmark levels/PerfStress.alvl
   ```

   On Windows, the folder is `out/build/msvc-ship-chiara/apps/game`.

What the benchmark does:

- **It waits for the level to load**, then draws the first point of the route
  for 3 seconds, so nothing that happens only once, such as preparing shaders,
  is measured.
- **It flies the whole route in 15 seconds**, scaling the paths' `seconds` to
  fit, then closes the game.
- **It uses fixed settings**, so runs on different days compare: a 1920 × 1080
  window, 8× MSAA with FXAA, the Ultra shadow tier, sky reflections at their
  defaults, ambient occlusion off, and no frame rate limit. The player's
  `options.json` is not changed. The tone map and frame snapping still come
  from it.
- **The level's simulation follows the route's clock**, so anything that moves
  is in the same place at the same point of every run.

It records a profiler session of the run in the `captures` folder, and the log
says where. Open it in Perfetto to read the frame times. The benchmark doesn't
print a summary of its own.

| Option | What it does |
|---|---|
| `--benchmark [level]` | Run the benchmark. Without a level, it uses `startupScene` from `game.json`. |
| `--benchmark-seconds <n>` | Fly the route in `n` seconds instead of 15. |
| `--benchmark-passes` | Also time each pass. As in the editor, this slows the frame a little. |
| `--benchmark-shots <dir>` | Instead of measuring, stop at points evenly spaced along the route and save a PNG picture of each in `dir`, as `shot-00.png`, `shot-01.png` and so on. |
| `--benchmark-shot-count <n>` | How many pictures `--benchmark-shots` takes. Default 24. |

A level with no camera paths, or none with `seconds` above 0, refuses to start
and the log says so.

`--benchmark-shots` is for checking that a change didn't alter the picture: run
it before and after, and compare the two sets of pictures. Each picture is taken
from exactly the same point both times.

### Measuring one view in the editor

The editor can also measure a level from one fixed view and print the result.
It works in any build, with or without the profiler:

```bash
./out/build/gcc-ship/apps/game/Assisi-GameEditor -l levels/PerfStress.alvl --capture --no-editor-visuals
```

This looks through the level's active camera, draws 120 frames to warm up,
measures the next 600, writes a report to the log, and closes. The report gives
the middle (median) frame time on the graphics card and on the processor, how
much they varied, and whether the graphics card kept a steady speed throughout.
`--no-editor-visuals` leaves out the editor's outlines and icons, so what is
measured is what the game draws.

| Option | What it does |
|---|---|
| `--capture [frames]` | Measure this many frames. Default 600. |
| `--capture-warmup <n>` | Frames drawn before measuring. Default 120. |
| `--capture-size <WxH>` | The resolution, such as `1920x1080`. |
| `--capture-camera <ex,ey,ez,tx,ty,tz>` | Stand the camera at `e`, looking at `t`, instead of using the level's camera. |
| `--capture-options <path>` | Use this options file instead of `options.json`, which is left alone. |
| `--capture-out <path>` | Also write the report as JSON to this file. |
| `--capture-image <path>` | Save a PNG of the frame after the last one measured. |
| `--capture-golden <dir>` | Compare that PNG with the picture accepted for this level on this graphics card, in `<dir>/<GPU>/<level>.png`, and fail if they differ. This is what the GPU tests do. |
| `--capture-passes` | Also time each pass. |
| `--vulkan-validation` | Check every request to the graphics card with the Vulkan validation layer, in any build, and fail if it finds an error. |

Run `Assisi-GameEditor -h` for the full list.
