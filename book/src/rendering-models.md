# Models

Anything solid you see in a level, from a crate to a building, is a **mesh**: a
surface made of triangles. An entity draws a mesh when it has a `Transform` and
a `MeshRenderer`. The mesh comes either from a model file you import or from
one of the engine's built-in shapes.

How each part of a mesh looks, its colour, how shiny or rough it is, and the
small bumps on it, comes from its **materials**: see
[Materials](rendering-materials.md).

## `MeshRenderer` fields

`MeshRenderer` is in `<Assisi/Runtime/Components.hpp>`:

| Field | Meaning |
|---|---|
| `mesh` | The mesh to draw: a model file, or a built-in shape. Empty draws a 1 m cube. |
| `materialOverrides` | Materials to use instead of the ones the model came with, one entry per material slot. See [Material slots](#material-slots). |
| `castsShadows` | Whether the mesh casts shadows. On by default. See [Shadows](rendering-shadows.md). |
| `hiddenFromOwnCamera` | Whether the mesh is hidden from a camera in the same blueprint instance. A player's body has it set, so the player doesn't see the inside of their own head while everyone else sees them. Its shadow is still drawn. See [Players](players.md). |

The entity's `Transform` places, turns and scales the mesh. A mesh can be
attached to another entity with a `Parent`, and it then moves with that entity.

## Built-in shapes

The engine has these shapes built in, for blocking out a level or testing. Pick
them with the **...** button on the `mesh` field; they are listed as `prim://`
entries.

| `mesh` | Shape | Triangles |
|---|---|---|
| `prim://cube` | A cube, 1 m on each side. | 12 |
| `prim://sphere-low`, `prim://sphere`, `prim://sphere-high` | A sphere made of rings, at three levels of detail. | 144, 576, 4096 |
| `prim://icosphere-low`, `prim://icosphere`, `prim://icosphere-high` | A sphere made of near-equal triangles, which looks rounder at the same triangle count. | 320, 1280, 20480 |
| `prim://cylinder`, `prim://cylinder-high` | A cylinder. | 96, 256 |
| `prim://quad` | A flat square, 1 m on each side, facing along +Z: a foliage card, a sign. Seen only from the front unless its material is double-sided. | 2 |

A built-in shape has one material slot. It draws with the engine's default
material, plain white and moderately rough, until you give it another in
`materialOverrides` (see [Material slots](#material-slots)). The test levels
give their cubes `materials/checker.amat` this way.

## Importing a model

The editor imports models in the **glTF** format, as `.gltf` or `.glb` files.
Most modelling programs, including Blender, export it.
[The editor](editor.md#adding-a-model) gives the steps: copy the model into
`assets/`, restart the editor or press **Reimport** in the asset browser, then
pick the model on a `MeshRenderer`'s `mesh` field.

What the import reads from the file:

- **Every mesh in the file's default scene**, combined into one model. Where
  each object sits in the file is kept, so a model made of several objects
  arrives assembled.
- **Its materials.** The editor writes one material file (`.amat`) per material
  in the model, next to the model file. See
  [Materials from a model](rendering-materials.md#materials-from-a-model).
- **Its textures**: the `.png` or `.jpg` files its materials name. They must be
  inside `assets/` too, usually next to the model.
- **Its skeleton**, if it has one, for animation. See [Animation](animation.md).
- **Collision pieces**, objects whose names start with `UCX_`, `UBX_` and so on.
  They are not drawn. See [Collision from a model](physics-model-collision.md).
- **Detail levels**, objects whose names end in `_LOD1`, `_LOD2` and so on. See
  [Detail levels](rendering-lod.md).

Things the import doesn't do yet:

- **Textures stored inside a `.glb`** are not read: the model imports with its
  material colours but no textures, and the log says so. Export as `.gltf` with
  separate texture files instead. In Blender that is the **glTF Separate**
  format. A `.glb` with its textures as separate files works.
- **Only PNG and JPEG textures** are read.
- **Compressed geometry** (Draco or meshopt) is refused.
- **Only the first set of texture coordinates** is used. A material that maps a
  texture through a second set loses that texture, with a warning.

A model loads in the background. Until it has loaded, it isn't drawn; until its
textures have loaded, it draws plain white. A model file that fails to load
draws as the 1 m cube, and the log says why.

## Material slots

A model's materials are numbered in the order the model uses them: slot 0,
slot 1 and so on. A chair might use slot 0 for its wooden frame and slot 1 for
its cushion. Each slot draws with the material the import made for it, unless
the `MeshRenderer` overrides it.

`materialOverrides` is a list with one entry per slot. Entry 0 replaces slot 0's
material, entry 1 slot 1's, and so on. An empty entry, or a slot past the end of
the list, keeps the model's own material. So to give one chair a red cushion,
leave entry 0 empty and set entry 1 to a red material.

In the inspector, `materialOverrides` shows one row per slot, labelled with the
model's name for that material. Each row has a box for the override, a **...**
button to pick a material file, and **Edit**, which opens the material the slot
draws with in the **Material** panel.

Overriding a slot changes only that entity. To change every chair, edit the
material the import made instead: with no override set, **Edit** opens that
one, and its tooltip warns that everything using the model shares it.
