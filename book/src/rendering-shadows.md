# Shadows

A shadow needs extra drawing. For each light that casts one, the engine draws
the scene again from the light's point of view into a **shadow map**: a picture
in which each pixel, called a **texel**, holds how far the nearest surface is
from the light in that direction. When the lit pass draws a surface, it looks
up the surface's position in the shadow map. If the map holds something nearer
the light, the surface is in shadow.

So a shadow costs a second drawing of the meshes the light reaches, and memory
for the map. This page explains which lights cast shadows, what the shadow
settings change, and what to turn down when shadows cost too much.

## What casts a shadow

Two switches decide it, both on by default:

| Field | Component | What turning it off does |
|---|---|---|
| `castsShadows` | Every light | The light shines through everything, and costs no shadow drawing. |
| `castsShadows` | `MeshRenderer` | The mesh still receives shadows, but casts none, and isn't drawn into any shadow map. |

A mesh's shadow follows its material: a masked material, such as a leaf with
holes cut by its texture, casts a shadow with the same holes. See
[Materials](rendering-materials.md).

Turning off `castsShadows` on a mesh is the cheapest saving there is. Good
candidates are small props whose shadow nobody notices, and meshes that are
always inside another mesh's shadow.

Each kind of light gets its shadow differently:

| Light | Its shadow |
|---|---|
| `DirectionalLight` | One set of **cascades** that follows the camera. Only one directional light casts shadows: the first the engine finds with `castsShadows` on. At night, the moon casts it instead of the sun. |
| `SpotLight` | One shadow map, the shape of its cone. |
| `PointLight` | Six shadow maps, one for each side of a cube around the light, because it shines in every direction. A point light's shadow costs about as much as six spot lights'. |

If you need a light that casts shadows in one direction, such as a ceiling lamp
lighting a room below it, a spot light is far cheaper than a point light.

## The sun's shadow: cascades

The sun lights the whole level, but one shadow map stretched over the whole
level would have texels metres wide, and every shadow would be a blur. Instead,
the area in front of the camera is cut into slices by distance, called
**cascades**. Each cascade has its own shadow map of the same size. Near the
camera a cascade covers a few metres, so its texels are small and its shadows
sharp; far away a cascade covers tens of metres, where the coarser shadows are
too small on screen to notice.

- **The sun's shadows end at the shadow distance**: 80 m by default, or the
  camera's `farZ` if that is shorter. Past it, surfaces in the sun are fully
  lit, and the last stretch before it fades out so there is no hard line.
- **Where two cascades meet**, the shadows change from finer to coarser. The
  engine blends the two over a third of each cascade, so the change is a
  gradual softening rather than a visible line.
- **A cascade is kept when nothing changed.** If the sun hasn't moved, the
  camera hasn't moved far, and no shadow-casting mesh in the cascade has moved,
  the engine reuses the cascade from the previous frame instead of drawing it
  again. A still scene under a still sun draws no sun shadows at all after the
  first frame. Moving meshes are drawn on top of the kept still ones, so one
  moving object doesn't make the whole cascade redraw.

The cost of the sun's shadow is roughly the number of cascades redrawn, times
the shadow-casting meshes in each.

## Spot and point light shadows

All spot and point light shadows share one large texture, called the **shadow
atlas**. Each shadow map is a rectangle in it, called a **tile**. A spot light
takes one tile and a point light six.

- **Tile size follows screen size.** A light whose reach fills half the screen's
  height gets a full-size tile (512 × 512 texels by default). Each time its
  reach halves on screen, it gets a tile half as wide, which leaves room for
  other lights.
- **Still shadows are kept.** As with the sun, a light whose surroundings
  haven't moved keeps its tile from the previous frame and costs nothing. Only
  meshes that are moving are drawn again. A mesh that stops moving is folded
  back into the kept shadow after it has been still for a second.
- **Shadows are drawn a few at a time.** At most 32 tiles are redrawn in one
  frame, most important first. When many lights need redrawing at once, for
  example on the first frame of a level or when the camera jumps somewhere new,
  some lights have no shadow for a few frames until their turn comes. They are
  never shown with an out-of-date shadow.

### Which lights get a shadow

The game allows a limited number of shadowed lights at once: by default 16 spot
lights and 4 point lights. When more shadow-casting lights than that are in
view, the engine ranks them by how much they contribute to the picture, which
is how much of the screen they light and how bright they are, and the rest are
drawn without shadows.

Two fields on `SpotLight` and `PointLight` change the ranking:

| Field | Meaning |
|---|---|
| `shadowPriority` | Raises or lowers the light's rank, from −8 to 8. Each step doubles or halves how important it counts as: `1` ranks it as if it were twice as important, `-1` half. Default 0. |
| `shadowAlwaysOn` | The light always keeps its shadow, whatever its rank. If the atlas is full, it gets a smaller tile rather than none. |

Use `shadowAlwaysOn` for the light whose shadow matters to the scene, such as
the lamp over a table where a conversation happens. The ranking only decides
anything when the limits are reached; in most levels every shadowed light gets
its shadow.

A light that is ranked out loses its shadow, but still lights the scene.

## Shadow settings

How detailed the shadows are is the player's choice, saved in `options.json`
under `shadows`. The editor's **F11** window edits them, in its **Sun Shadows**
and **Local Light Shadows** sections; every key is listed in
[Graphics settings](rendering-settings.md#shadows).

### Quality tiers

The four buttons **Low**, **Medium**, **High** and **Ultra** set the main
quality settings at once. Medium is the default. Changing any setting a tier
sets afterwards shows **Custom**.

| Setting | Low | Medium | High | Ultra |
|---|---|---|---|---|
| Sun cascades | 4 | 4 | 4 | 6 |
| Sun shadow map size | 1024 | 2048 | 4096 | 4096 |
| Sun depth format | 16-bit | 32-bit | 32-bit | 32-bit |
| Sun shadow distance | 40 m | 80 m | 80 m | 100 m |
| Atlas size | 2048 | 4096 | 4096 | 8192 |
| Largest tile | 256 | 512 | 512 | 512 |
| Filter, sun and local | 1 tap | 3×3 | 5×5 | 16-tap disk |
| Shadowed spot lights | 8 | 16 | 32 | 128 |
| Shadowed point lights | 2 | 4 | 8 | 32 |
| Contact hardening | Off | Off | Off | Sun |
| GPU memory for shadow maps | 32 MiB | 192 MiB | 576 MiB | 1024 MiB |

The memory is only used while the level has something that casts: the sun's
cascades exist only while a directional light casts shadows, and the atlas only
while a spot or point light does.

### What each setting changes

- **Shadow map size.** Bigger maps give sharper shadows and cost memory, but
  very little drawing time.
- **Cascades.** More cascades make the step between near and far shadows
  smaller, but each one is another drawing of the meshes in it whenever it is
  redrawn.
- **Shadow distance.** How far the sun's shadows reach. Spreading the same
  cascades over a longer distance makes every cascade coarser.
- **Filter.** How a shadow's edge is softened. The engine compares a small
  patch of the shadow map around the lookup instead of a single texel, and
  averages the results; this is called **percentage-closer filtering**, or
  **PCF**. `1 tap` gives the hardest, most jagged edge; `3x3 PCF` and
  `5x5 PCF` compare 9 and 25 texels; `Vogel (16 tap)` compares 16 texels
  scattered in a disk, turned differently at every pixel, which looks softer
  than a grid. More taps cost more for every pixel in shadow range.
- **Contact hardening.** Makes a shadow sharp where an object touches the
  ground and softer further from it, as real shadows are. It adds a search of
  the shadow map to every shadowed pixel. **Sun** uses it for the sun only, and
  **Sun + Local Lights** for every light. **Source Radius** sets how large spot
  and point lights are taken to be for this: bigger lights give softer shadows.
- **Depth format.** 16-bit shadow maps take half the memory of 32-bit ones and
  are a little less precise. Spot and point lights use 16-bit at every tier,
  which is enough for their short range.

### When shadows look wrong

A flat surface never shadows itself, however low the light that reaches it:
the lookup compares each texel it reads against the surface's own slope, so
the surface beside the pixel is never taken for something in front of it. A
smooth-shaded model doesn't either, where its shape matches its smoothing; see
[Smooth shading and shadows](rendering-smooth-shading.md).
Three settings, the **biases**, move each lookup slightly to cover what that
slope can't: curved surfaces, which bend away from it, and the shadow map's
limited precision. Their defaults suit most levels. Change them only to fix
one of these:

| What you see | Name | Fix |
|---|---|---|
| Stripes or speckles of shadow across a lit surface. | **Shadow acne**: the surface is shadowing itself. | Raise **Depth Bias**, **Slope Bias** or **Normal Offset** a little. |
| Stripes or misplaced shadow on a smooth-shaded model where the light turns away from it, and a warning in the log when it was imported. | The model is smoothed over corners too sharp for it. | See [When the importer warns about a model](rendering-smooth-shading.md#when-the-importer-warns-about-a-model). |
| Shadows that start a little away from the object casting them, so the object seems to float. | **Peter-panning**: too much bias. | Lower the same three. |
| A visible line in the sun's shadows at some distance from the camera. | A cascade boundary. | Raise **Cascade Blend**, or add a cascade. |
| A lamp has no shadow. | Ranked out, no room in the atlas, or waiting its turn. | Select the light: the inspector shows why (see below). |

### Finding out what happened to a light's shadow

Select a spot or point light in the editor. Under its fields, the inspector
shows a line such as **Shadow: shadowed at 512**: whether the light has a
shadow this frame, and how big its tile is. Hover over it for an explanation
and what to change:

| Shown | Meaning |
|---|---|
| `shadowed` | It has a full-size tile. |
| `demoted` | It has a smaller tile than it asked for, because the atlas had no room. A larger atlas, or fewer shadowed lights, gives it back. |
| `deferred` | No shadow this frame: the redraw limit hasn't reached it yet. Normal for a frame or two; if it stays, raise **Redraw Budget**. |
| `atlas full` | No shadow: the atlas had no room even for its smallest tile. Raise the atlas size or lower the caps. |
| `capped out` | No shadow: the ranking put it below the limit. Raise **Shadowed Spots** or **Shadowed Points**, raise its `shadowPriority`, or set `shadowAlwaysOn`. |
| `shadows off` | Spot and point light shadows are turned off in the settings. |

The F11 window's **Shadowed Lights** list shows the same for every light at
once. See [Looking at rendering problems](rendering-debugging.md).

## Making shadows cheaper

What you can do as you build the level:

- Turn off `castsShadows` on meshes whose shadow doesn't matter, and on lights
  whose shadow nobody will notice: fill lights, lights high up in a large room,
  lights far from where the player goes.
- Use a spot light instead of a point light where the light only needs to shine
  one way.
- Keep each light's `radius` no larger than it needs to be. A shadow is drawn
  for every mesh inside the light's reach.
- Keep shadow-casting meshes still where you can. A mesh that moves makes every
  shadow it is in redraw its moving part every frame.

What the player can turn down:

- **The tier**, or **Cascades** and **Distance** on their own: fewer meshes
  drawn into the sun's shadow.
- **Contact Hardening**, which costs a search in every shadowed pixel.
- **Filter**: fewer taps for every shadowed pixel.
- **Shadowed Spots** and **Shadowed Points**: fewer lights with a shadow to
  draw.
- **Cast Shadows**, in either section, to turn the sun's shadows or the spot and
  point light shadows off entirely.
