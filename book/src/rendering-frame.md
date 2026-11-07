# How a frame is drawn

Each frame, the engine draws the picture in a fixed series of steps called
**passes**. Each pass is one job for the graphics card, such as drawing the
shadow maps or drawing every mesh. Knowing the passes tells you where the time
goes, and which setting or component to change when a frame takes too long.

You don't call or configure any of this yourself. The engine runs the same
passes for every level; your levels and the player's settings decide how much
work each one does.

## Preparing the frame

Before the graphics card draws anything, the engine works out on the processor
what the frame contains:

1. **Where everything is.** Each entity's `WorldMatrix` is updated from its
   `Transform` and its parents', for entities that moved.
2. **The sky.** Where the sun and moon are, from the `TimeOfDay`, and which one
   lights the world.
3. **The lights.** Every light component in the level is gathered.
4. **Detail levels.** Which [LOD](rendering-lod.md) each mesh is drawn at.
5. **Shadow casters.** Which meshes go into which shadow maps, and which shadow
   maps can be kept from the last frame.
6. **The draw list.** Meshes outside the camera's view are left out, which is
   called **culling**, and the rest are sorted so that copies of the same mesh
   with the same material are drawn together in one go.

The cost of this grows with the number of entities with a `MeshRenderer` and
with how many of them move.

## The passes, in order

The name in brackets after each pass is what it is called in a profiler capture
(see [Looking at rendering problems](rendering-debugging.md#the-profiler)).

### 1. Clear (`clear-targets`)

Fills the picture with `clearColor` from `game.json`, and empties the **depth
buffer**, which records how far away the surface drawn at each pixel is, so a
surface behind another is hidden. Almost free.

### 2. Skinning (`skin`)

Moves the vertices of every animated, skinned mesh to its pose for this frame,
so every later pass draws the posed mesh. Only runs for entities with a
`SkinnedMesh`; its cost grows with how many vertices they have. See
[Animation](animation.md).

### 3. Light grid rebuild (`cluster-rebuild`)

Rebuilds the grid of **clusters** that point and spot lights are sorted into
(see [What lights cost](rendering-lights.md#what-lights-cost)). It only runs on
a frame when the window changes size or the active camera's `fovDegrees`,
`nearZ` or `farZ` change.

### 4. Sky reflections (`sky-probe`)

Draws the sky into the reflection probe, the six pictures shiny surfaces
reflect. It runs only when the sun or moon has moved more than the **Rebake
Angle**, so a still sky draws it once. Only in a level with a `Skybox`, and
only with sky reflections on.

- **Changed by:** the `environment` settings; `Skybox`, `TimeOfDay`.

### 5. Sun shadows (`shadow-cascades`)

Draws the sun's shadow **cascades**: the shadow-casting meshes in each one, as
seen from the sun. Cascades in which nothing changed are kept from the last
frame; moving meshes are drawn over a kept cascade on their own. Its cost
grows with the number of cascades redrawn and the meshes in each. See
[Shadows](rendering-shadows.md).

- **Changed by:** the `shadows.sun` settings; `DirectionalLight.castsShadows`;
  `MeshRenderer.castsShadows`; anything moving in the sun.

### 6. Spot and point light shadows (`shadow-atlas`)

Draws the shadow maps of the spot and point lights that need it into the shadow
atlas, up to the redraw budget. Lights whose surroundings are still keep their
maps and cost nothing. A point light costs six times a spot light.

- **Changed by:** the `shadows.local` and `shadows.selection` settings; each
  light's `castsShadows`, `radius`, `shadowPriority` and `shadowAlwaysOn`;
  `MeshRenderer.castsShadows`; anything moving near a shadowed light.

### 7. Light sorting (`light-cull`)

Sorts every point and spot light into the clusters it reaches, so that each
pixel later checks only the lights near it. Small; its cost grows with the
number of lights.

### 8. Depth prepass

Draws every mesh in view, but only how far away each pixel is, with no
lighting or colour. The lit pass then shades each pixel once, for the surface
in front, rather than once for every surface drawn over it. Its cost grows with
the number of triangles in view, which [detail levels](rendering-lod.md) keep
down.

- **Changed by:** the number of meshes and triangles in view; masked materials,
  which cost more.

### 9. Ambient occlusion (`scene-distance`, `ssao-occlusion`, `ssao-blur`)

Finds the creases and corners on screen from the prepass's depth, and works
out how much to darken the ambient light in them. Three passes over the whole
screen. Off by default.

- **Changed by:** the `ambientOcclusion` settings; the resolution.

### 10. Lit pass (`draw-scene`)

Draws every mesh in view again, this time working out the colour of each pixel:
it reads the material's maps, adds the light of every directional light and of
the point and spot lights in the pixel's cluster, looks each one up in its
shadow map, adds the sky's ambient light and reflections, and last fades the
result into the level's [fog](rendering-sky.md#fog). This is usually the most
expensive pass. Its cost grows with the number of pixels, the lights reaching
each pixel, and the shadow filter.

- **Changed by:** the resolution and MSAA; the number and `radius` of lights;
  the number of directional lights; the shadow `filter` and contact hardening;
  every material; a `Fog`.

### 11. Sky (`sky`)

Draws the sky into every pixel that nothing else covered. Only in a level with
a sky, or a foggy level with none, where it fills those pixels with the fog's
colour. With fog on, it draws the horizon's colour below the horizon instead of
the ground's. Its cost grows with how much sky is on screen.

- **Changed by:** `Skybox`; `Fog`; the resolution.

### 12. Post-processing (`post-process`, `post-process-output`)

Turns the drawn scene into the finished picture:

1. With MSAA, each pixel's samples are combined into one.
2. With automatic exposure, the picture's brightness is measured
   (`pp-exposure`).
3. **Bloom**, if it is on, spreads a soft glow around the brightest light
   (`pp-bloom`).
4. The **tone map** balances the colours, applies the exposure, and turns light
   values into screen colours, with the level's contrast and saturation.
5. In the editor, the selection outline, icons and wire shapes are drawn.
6. FXAA, if it is on, or a copy to the screen.

- **Changed by:** the `antiAliasing`, `bloom` and `toneMap` settings; a
  `PostProcessVolume`. See [The level's look](rendering-post-processing.md).

### 13. User interface (`game-ui`)

Draws the game's [screens](user-interface.md): menus and HUDs. They are drawn
after the tone map, so their colours are exactly as you set them. In the editor,
the editor's own windows are drawn after this.

## Which pass a setting or component changes

| You change | Pass |
|---|---|
| Resolution, MSAA samples | Lit pass, sky, prepass, post-processing |
| `antiAliasing.mode` | Post-processing, and MSAA's cost in the lit pass |
| `toneMap` | Post-processing (free) |
| `bloom` | Post-processing |
| A `PostProcessVolume` | Post-processing; automatic exposure adds its measuring passes |
| `shadows.sun` | Sun shadows; the filter in the lit pass |
| `shadows.local`, `shadows.selection` | Spot and point light shadows; the filter in the lit pass |
| `shadows.pcss` | Lit pass |
| `environment` | Sky reflections |
| `ambientOcclusion` | Ambient occlusion |
| A `MeshRenderer` | Preparation, prepass, lit pass, and every shadow it falls in |
| `MeshRenderer.castsShadows` | Sun shadows, spot and point light shadows |
| A `PointLight` or `SpotLight` | Light sorting, lit pass, and its shadow if it casts one |
| A `DirectionalLight` | Lit pass, everywhere; sun shadows if it casts |
| `Skybox`, `TimeOfDay`, `Sun`, `Moon` | Sky, sky reflections, sun shadows |
| `Fog` | Lit pass, sky; and, through the draw distance, everything `Camera.farZ` changes |
| `Camera.farZ` | Light grid; how far the sun shadows reach |
| A material's `Alpha` set to `Mask` | Prepass, lit pass, shadows |
| A `SkinnedMesh` | Skinning |
