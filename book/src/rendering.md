# Rendering

**Rendering** is drawing the picture the player sees, many times a second. Each
picture is called a **frame**. You don't write code to draw a level: you place
entities with the right components, and the engine draws them every frame.

| To get | Give an entity |
|---|---|
| Something solid on screen | A `Transform` and a `MeshRenderer` |
| A point of view | A `Transform` and a `Camera` |
| Light | A `DirectionalLight`, a `PointLight` or a `SpotLight` |
| A sky, and a sun that moves | A `Skybox`, `TimeOfDay` and `Sun` beside the `DirectionalLight` |

How each surface looks comes from its **material**, a file that says what it is
made of. How detailed the shadows and edges are is the player's choice, in
their graphics settings.

This page gets a lit object on screen. The pages after it explain each part:

| Section | What it covers |
|---|---|
| [Models](rendering-models.md) | `MeshRenderer`, the built-in shapes, importing glTF models, and material slots. |
| [Materials](rendering-materials.md) | Material files, what each texture map does, and masked and double-sided materials. |
| [The camera](rendering-camera.md) | The `Camera` component, which camera draws, the draw distance, and camera paths for benchmarks. |
| [Lights](rendering-lights.md) | The three light components, how bright to make them, and what they cost. |
| [Shadows](rendering-shadows.md) | Which lights cast shadows, how the sun's and the lamps' shadows work, quality tiers, and fixing shadows that look wrong. |
| [Sky and time of day](rendering-sky.md) | `Skybox`, `TimeOfDay`, `Sun` and `Moon`, and the light the sky casts on the world. |
| [Detail levels](rendering-lod.md) | Drawing simpler versions of a model further away, and how to make them. |
| [The level's look](rendering-post-processing.md) | `PostProcessVolume`: a level's exposure, contrast, saturation, white balance and bloom, for the whole level or one place in it. |
| [Graphics settings](rendering-settings.md) | Every graphics setting in `options.json`, and what each costs. |
| [How a frame is drawn](rendering-frame.md) | The passes in order, what each costs, and what changes each one. |
| [Drawing far from the origin](rendering-far.md) | How the engine draws precisely hundreds of kilometres out, and the draw distance. |
| [Looking at rendering problems](rendering-debugging.md) | The editor's debug views and readouts, common problems, and measuring how fast a level draws. |

## Words used in this chapter

| Word | Meaning |
|---|---|
| **Mesh** | A surface made of triangles: the shape of a crate, a character or a building. |
| **Model** | A file holding one or more meshes, their materials and textures, made in a modelling program. |
| **Material** | What a surface is made of: its colour, whether it is metal, how rough it is. |
| **Texture** | An image laid over a mesh's surface, such as wood grain or a brick pattern. A **texel** is one pixel of a texture. |
| **Shadow map** | A picture of the scene from a light's point of view, used to find what is in shadow. |
| **Pass** | One step of drawing a frame, such as drawing the shadow maps, or drawing every mesh. |
| **Draw distance** | How far the camera can see. Nothing further is drawn. |

## Try it: a lit shape seen through a camera

### Step 1: Open a level

Start the editor on the shadow test level:

```bash
./out/build/gcc-dev/apps/game/Assisi-GameEditor -l levels/ShadowCheck.alvl
```

It has a floor, a few shapes, a point light, a spot light and a camera, but no
sun: it is lit only by its two lamps.

### Step 2: Add a shape

1. Click **+** in the **Entities** panel and add a `Transform`. The entity is
   placed 5 m in front of you.
2. Add a `MeshRenderer`. A cube appears.
3. Click the **...** button on its `mesh` field and pick `prim://sphere`. The
   cube becomes a sphere.

### Step 3: Add the sun

1. Add another entity, and give it a `DirectionalLight`. It needs no
   `Transform`: sunlight comes from the same direction everywhere.
2. The level lights up, and every shape casts a shadow straight down, because
   the light's `direction` points straight down.
3. Add a `Skybox` to the same entity. A blue sky appears behind the level.

### Step 4: Look through the camera

Press **F5** to play. The view jumps to the level's own camera, the entity
called `Camera`: while the game runs you see through the level's active camera,
not the one you fly in the editor. Press **F7** to stop, and the editor's view
comes back.

To keep your changes, save the level under a new name with **Save As** in the
**Levels** panel.
