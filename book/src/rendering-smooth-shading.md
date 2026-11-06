# Smooth shading and shadows

A low-poly model is usually **smooth-shaded**: each corner of each triangle
carries a **normal**, the direction the surface faces at that point, and the
lighting blends the corners' normals across the triangle. That is what makes a
16-sided cylinder look round instead of faceted.

Its shadow is a different matter. The shadow map holds the model's flat
triangles, not the round shape the lighting shows. Near the edge between the
lit side and the dark side, some triangles face away from the light while the
blended normal still faces it. Those triangles really are in the shadow of
their neighbours, so without help the engine would draw a stripe of shadow on
each one along that edge.

## What the engine does about it

Near that edge, the engine tests each pixel's shadow from the curved surface
the normals describe, a little outside the flat triangle, instead of from the
triangle itself. The stripes disappear, and you don't need to set anything up.
It works the same for animated models, scaled models and every
[detail level](rendering-lod.md).

Two side effects remain, both inside the dim strip where the light only just
reaches:

- A shadow another object casts onto the model there can sit slightly off
  where it was cast, more so the fewer sides the model has.
- Two low-poly objects that almost touch can let a little light through each
  other there.

## Foliage: `ShadowsFollowSmoothing`

Leaves and grass are usually flat cards whose normals are bent outward so the
whole plant shades like one round bush. Shadows that followed those normals
would let light through the plant's own shade. A material's
`ShadowsFollowSmoothing` field decides whether its shadows follow the
smoothing:

| Field | Meaning | Default |
|---|---|---|
| `ShadowsFollowSmoothing` | On, shadows near the edge of the light follow the curve the normals describe, so smooth-shaded models get no stripes. Turn it off for foliage cards and other surfaces whose normals don't match their shape on purpose. | On |

When you import a model, the editor turns it off for masked (cutout) and
double-sided materials, since foliage is made of those. Turn it back on in the
`.amat` if a material of yours is neither.

## When the importer warns about a model

The engine can only follow normals that roughly match the model's shape. When
you import a model, it checks each corner, and if a corner's normal points
far away from the triangles it belongs to, the log shows a warning like this:

```text
MeshImporter: 'models/cone.glb' (material 'Shape') is smooth-shaded over corners too sharp for it, so it bulges 24% of its size off its flat faces and shadows there will look badly wrong. The editor's Smoothing debug view shows where. How to fix it: https://dafrancc.github.io/Assisi/rendering-smooth-shading.html
```

It means the model is smoothed across a corner that should be sharp. The
curve those normals describe bulges far out from the real shape, and shadows
near the edge of the light follow that bulge. The percentage is how far it
bulges, as a share of the whole model's size:

| Bulge | What the warning says | What you see |
|---|---|---|
| Under 1% | Nothing: there is no warning. | Too small to notice. |
| 1% to 5% | Shadows there "may look slightly wrong". | Shadows near the edge of the light slightly off, close up. |
| 5% and more | Shadows there "will look badly wrong". | Shadows near the edge of the light visibly misplaced or broken up. |

The usual causes:

- **A point where many faces meet**, such as a cone's tip, sharing one vertex.
  Its normal points straight out of the tip while every face around it leans
  away.
- **Flat parts smoothed into each other**, such as a cylinder whose top shares
  its rim vertices with the side, or a box smoothed all over.
- **Too few sides.** A cylinder or sphere with four sides or fewer is closer to
  a box than to the round shape its smoothing describes.

## Try it: find and fix sharp smoothing

This uses Blender; other modelling tools have the same operations under
similar names.

### Step 1: See where it is

In the editor, press **F11** and set **Debug View** to **Smoothing**. Every
model turns grey. The places the warning is about turn red: dark red where the
smoothing is just too sharp, bright red where it is far too sharp. Materials
with `ShadowsFollowSmoothing` off are blue-grey, because they aren't checked.

### Step 2: Fix it in the model

Open the model in Blender and fix each red area in whichever of these ways
fits it:

1. **Make edges between flat parts sharp.** In Edit Mode, select the edges
   where two parts meet at an angle, such as the rim of a cylinder or the edges
   of a box, and choose **Edge > Mark Sharp**. Each side of a sharp edge gets
   its own normals, so the smoothing stops there. In Blender older than 4.1,
   also tick **Auto Smooth** under **Object Data > Normals**, or the marks are
   ignored.
2. **Blunt a point.** A tip where many faces meet can't be smoothed with one
   normal. Cut it off with a very small flat cap, and mark the cap's edges sharp.
   Or, if it should look faceted, mark the edges that meet at the tip sharp.
3. **Add sides.** Give a cylinder or sphere at least five sides around.
4. **Turn the check off for foliage.** If the red is on leaves or grass, set
   `ShadowsFollowSmoothing` to off in that material instead.

### Step 3: Check it

Export the model again over the old file. The editor imports it again: the
warning no longer appears in the log, and the red is gone from the
**Smoothing** view. Set **Debug View** back to **Off**.

## What smoothing can't fix

- **Saddle shapes**, curved one way across and the other way along, like the
  inside of a ring, are followed less exactly than round ones. On a very coarse
  ring under a very low light, a faint stripe can remain there. More sides
  remove it.
- **Normal maps** add bumps the shadow map knows nothing about, so a bumpy
  surface can still show a thin dark fringe at the edge of the light.
