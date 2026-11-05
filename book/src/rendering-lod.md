# Detail levels

A rock 200 m away covers a few pixels on screen, but the graphics card still
has to process all of its thousands of triangles. **Level of detail**, or
**LOD**, fixes this: a model carries several versions of itself, each with fewer
triangles than the one before, and the engine draws a simpler one the smaller
the model is on screen.

The versions are numbered. **LOD0** is the full-detail model, **LOD1** the first
simpler one, and so on. The engine only uses levels that the model file
contains; it does not simplify meshes itself.

## How the engine picks a level

Each frame, for each mesh, the engine measures how tall the mesh is on screen.
It takes the mesh's **bounding sphere**, the smallest ball that holds the whole
model, and divides its height on screen by the screen's height.

| The mesh is at least this much of the screen's height | It draws |
|---|---|
| 1/8 | LOD0 |
| 1/16 | LOD1 |
| 1/32 | LOD2 |
| 1/64 | LOD3 |

Each further level takes over at half the size again. A mesh smaller than all of
these draws the last level it has: a mesh is never left out for being small.

For a model whose bounding sphere has a 1 m radius, seen through the default
60° field of view, that is:

| Level | Drawn up to |
|---|---|
| LOD0 | about 14 m |
| LOD1 | about 28 m |
| LOD2 | about 55 m |
| LOD3 | about 110 m |

A model's last level is drawn from where it takes over, at any distance. The
distances scale with the model's size: a model twice as big switches twice
as far away. A narrower field of view, such as a sniper's scope, makes
everything bigger on screen, so finer levels are used further out.

Things to know:

- **A mesh near a switch point doesn't flicker between levels.** Going back to
  a finer level needs the mesh to be 2% bigger on screen than where it left it.
- **The switch points are the same for every model.** There is no setting per
  model or per level yet, and none in `options.json`.
- **Shadows use the same levels.** Spot and point light shadows draw each mesh
  at the level it has on screen. The sun's shadow may draw a mesh one level
  simpler than on screen, where the shadow's texels are too coarse to show the
  difference.
- **A model with one level** is always drawn in full.

## Making a model with detail levels

Put every level in the same model file, as separate objects, and name them by
level: an object whose name ends in `_LOD1` is level 1, `_LOD2` is level 2, and
so on.

```
Rock_LOD0    the full-detail rock
Rock_LOD1    about half the triangles
Rock_LOD2    about a quarter
Rock_LOD3    about an eighth
```

The rules:

- **The suffix must be the end of the name**, after the last `_`. `Rock_LOD1`
  is level 1; `Rock_LOD1_old` is level 0. Upper or lower case both work.
- **An object with no `_LOD` suffix is level 0.** Don't leave the original,
  unsuffixed object in the file beside `Rock_LOD0`, or both are drawn as level
  0, one inside the other.
- **A level can be several objects.** `Body_LOD1` and `Wheel_LOD1` are both in
  level 1.
- **Keep every level in the same place.** The levels replace each other where
  they stand, so model them on top of each other, not side by side. The
  bounding sphere the engine measures holds all of them.
- **Number the levels without gaps.** Levels 0, 1 and 3 still work, used in that
  order, but the log warns.
- **Halve the triangles at each level.** A level is used at half the size of the
  one before, so half the triangles is about right.
- **Use the same materials on every level**, so the model doesn't change colour
  when it switches.

The engine reads either the object's name or its mesh's name, whichever has the
suffix, because modelling tools export one or the other.

### Making the levels in Blender

`scripts/blender_lod_gen.py` makes levels for you in Blender. Save your `.blend`
file, select every object of the model, and run the script from Blender's
**Scripting** tab. It makes LOD0 to LOD4, each simplified further than the one
before, and exports them as `<name>/<name>.gltf` beside the `.blend` file, with
separate texture files. `<name>` is the object you selected last. Copy that
folder into `assets/`.

Check the simpler levels by eye: automatic simplification can wreck thin parts
and silhouettes. Fix them by hand where it matters.

## Looking at the levels in the editor

Select an entity with a model and look at its `MeshRenderer` in the inspector.
It shows which level is drawn and how big the mesh is on screen, for example
**LOD1 (LOD0-LOD3)**. **Pin this instance** fixes that one entity on a level of
your choice, so you can judge how it looks in place. The pin is never saved.

The **F11** window's **Rendering** section has controls for the whole view:

| Control | What it does |
|---|---|
| **Mesh LOD** | Untick to draw every mesh at LOD0. |
| **LOD Bias** | Multiplies every mesh's measured size. Above 1 keeps finer levels further away; below 1 switches sooner. |
| **Force LOD** | Draws every mesh at one level. `Auto` is normal selection. |
| **Shadow LOD** | Untick to draw the sun's shadows at the same level as on screen. |

Below them, a count of how many meshes were drawn at each level. These controls
only change the editor's view; they aren't saved, and a game always uses the
normal selection.
