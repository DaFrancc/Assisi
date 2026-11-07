# Materials

A **material** says what a surface is made of: its colour, whether it is metal,
how rough or polished it is, and small details such as scratches and bumps.
Light then falls on it the way it would on the real thing. This is called
**physically based** shading: you describe the material, not how it should look
under each light, and it looks right under all of them.

A material is a `.amat` file in `assets/`. A model's materials are made for you
when you import it; you can also make your own in the editor's **Material**
panel and put them on any mesh with `materialOverrides` (see
[Material slots](rendering-models.md#material-slots)).

## Try it: a red plastic cube

Open the editor on `levels/Test.alvl`.

### Step 1: Open the cube's material

Select one of the cubes. In its `MeshRenderer`, `materialOverrides` has one row,
for slot 0, set to `materials/checker.amat`. Click **Edit** on that row. The
**Material** panel opens on `checker.amat`.

Every cube in the level uses `checker.amat`, so don't change it. Make a copy
instead.

### Step 2: Make a copy

Press **Duplicate**. The panel opens the copy, `materials/checker_1.amat`, and
ticks **Live preview**, which shows the material you are editing on the cube
you started from.

### Step 3: Change it

1. In the `BaseColorTexture` row, clear the path. The checker pattern goes, and
   the cube turns plain white.
2. Click the colour square of `BaseColorFactor` and choose a red.
3. Set `MetallicFactor` to `0` and `RoughnessFactor` to `0.3`. The cube now
   looks like shiny red plastic, with a highlight that moves as you move the
   camera.
4. Press **Save**.

### Step 4: Assign it

Live preview only shows the material while the panel is open. To keep it, click
the **...** button on the cube's slot 0 row, pick `materials/checker_1.amat`, and
save the level.

## The material file

A `.amat` file is JSON. This is `assets/materials/checker_hole.amat`, a
checkered material with holes cut in it:

```json
{
  "version": 1,
  "type": "MaterialData",
  "BaseColorFactor": [1.0, 1.0, 1.0, 1.0],
  "BaseColorTexture": {
    "guid": "e9178367-c718-4df6-8b43-25308bd7bbbd",
    "path": "textures/checker_hole.png"
  },
  "MetallicFactor": 0.0,
  "RoughnessFactor": 0.6,
  "NormalScale": 1.0,
  "OcclusionStrength": 1.0,
  "EmissiveFactor": [0.0, 0.0, 0.0],
  "Alpha": 1,
  "AlphaCutoff": 0.5,
  "DoubleSided": true
}
```

- **A field left out keeps its default**, and a field the engine doesn't know is
  ignored. A field with the wrong type, such as text where a number goes, stops
  the whole file loading: the log names the field, and the surface draws plain
  white.
- **A texture is named by its `guid`**, the id in the texture's `.aast` file. The
  `path` is only there for people reading the file; the editor rewrites it when
  it saves.
- **Colours are linear**: the values the lighting works in, from 0 to 1, not
  the sRGB values a paint program shows. A paint program's middle grey, 128 out
  of 255, is about 0.22 in linear. The editor's colour picker shows and edits the
  linear values.

The **Material** panel edits the same fields, under the same names. Its **New**
button makes `materials/Material.amat`, or a new file beside the material that
is open.

## Colour, metal and roughness

| Field | Meaning | Default |
|---|---|---|
| `BaseColorFactor` | The surface's colour, as red, green, blue and alpha from 0 to 1. Multiplies `BaseColorTexture`. | White |
| `MetallicFactor` | `0` for anything that isn't metal: wood, stone, plastic, skin, paint. `1` for bare metal. Values between are for blending the two, such as rust on iron. | `1` |
| `RoughnessFactor` | How rough the surface is. `0` is a mirror polish, with sharp reflections; `1` is completely matte, like chalk. Values below 0.04 are treated as 0.04. | `1` |

A new material starts fully metallic and fully rough, which looks like dull
grey metal. Set `MetallicFactor` to `0` for anything that isn't metal.

The base colour of a metal is the colour of its reflections: yellow for gold,
near white for silver. The base colour of anything else is the colour you would
call it.

## Texture maps

A **texture** is an image laid over the mesh's surface. Each texture field of a
material is a **map** that varies one property across the surface, so a single
material can be rusty in some places and polished in others.

Every map is optional. Without one, the factor alone sets the property for the
whole surface. With one, the map's value at each point is multiplied by the
factor.

| Map | What it does | Read from |
|---|---|---|
| `BaseColorTexture` | The colour at each point. Its alpha is used by masked materials (see below). | The colour and alpha channels, as sRGB. |
| `MetallicRoughnessTexture` | Metal and roughness at each point. | Roughness from the green channel, metal from the blue, as glTF exports them. |
| `NormalTexture` | A **normal map**: small bumps, dents and grooves, without adding triangles. Each texel's colour says which way the surface faces at that point, and the lighting follows it. | Red and green. |
| `OcclusionTexture` | **Ambient occlusion**: how hidden each point is from the light around it, such as the inside of a crease. Black is fully hidden. | The red channel. |
| `EmissiveTexture` | Light the surface gives off itself: a screen, a lit window, glowing runes. | The colour channels, as sRGB. |

The factors that go with the maps:

| Field | Meaning | Default |
|---|---|---|
| `NormalScale` | How strong the normal map's bumps are. `0` flattens them. | `1` |
| `OcclusionStrength` | How much the occlusion map darkens. `0` turns it off. | `1` |
| `EmissiveFactor` | The colour and brightness of the glow. Multiplies `EmissiveTexture`. Values above 1 glow brighter. Black is no glow. | Black |

Things to know about the maps:

- **The occlusion map only darkens ambient light**, the light from the sky
  around the surface. It doesn't darken the sun, lamps or spot lights, which
  shadows handle.
- **An emissive surface glows, but lights nothing else.** It shows at full
  brightness in the dark, but doesn't light the floor in front of it. Put a
  `PointLight` next to it for that.
- **One image can be both the metal-and-roughness map and the occlusion map.**
  Many tools pack occlusion into the red channel of the metal-and-roughness
  image, and a glTF import binds that one file to both. Any other image used as
  two kinds of map, such as a colour map that is also a normal map, fails the
  cook, because each kind is compressed differently: copy the image and use one
  copy for each. A packed image is read once for both maps, which is a little
  cheaper than two images. `levels/OcclusionMapCheck.alvl` shows a sphere with
  no occlusion map, one with its own, and one with it packed.
- **A map that can't change the picture isn't read.** With `EmissiveFactor`
  black, the emissive map is skipped, and with `OcclusionStrength` at `0`, the
  occlusion map is.
- **Textures are PNG or JPEG files.** The cook compresses them and makes their
  smaller versions for distance, called **mipmaps**, for you.

## Masked materials

A **masked** material cuts holes in the surface wherever the base colour's alpha
is low: leaves on a branch, a chain-link fence, a torn flag. Each point is
either drawn or not; there is nothing in between.

| Field | Meaning | Default |
|---|---|---|
| `Alpha` | `Opaque` ignores alpha. `Mask` cuts out every point whose alpha is below `AlphaCutoff`. `Blend` makes the surface see-through (see [See-through materials](#see-through-materials)). In the file, `0` is `Opaque`, `1` is `Mask` and `2` is `Blend`. | `Opaque` |
| `AlphaCutoff` | The alpha below which a point is cut out, from 0 to 1. | `0.5` |

The alpha is the base colour texture's alpha times the alpha of
`BaseColorFactor`. The holes are cut in shadows too: a masked leaf casts a
leaf-shaped shadow.

Masked materials cost a little more to draw than opaque ones, so use `Mask` only
on materials that have holes. The holes themselves are not lit: the
[depth prepass](rendering-frame.md#9-depth-prepass) finds them, and the lit pass
skips them. `levels/PerfCutouts.alvl`, rows of cards with a hole in each, is the
level to measure them on.

## See-through materials

A **see-through** material shows the scene behind the surface, tinted by the
surface's colour: glass, water, ice, a soap bubble. Set `Alpha` to `Blend`. The
alpha, which is the base colour texture's alpha times the alpha of
`BaseColorFactor`, says how much the surface's own colour covers what is behind
it: at `0` you see only what is behind, at `1` nothing of it.

Open `levels/BlendCheck.alvl` to see them. It has three tinted panes that
overlap each other and the sky, a clear glass sphere on the left, a glass box on
the right, a glowing pane, and two panes that cross each other.

Things to know about see-through materials:

- **Reflections keep their full strength.** Alpha fades the surface's colour and
  glow, but not its highlights or its reflection of the sky. The clear sphere in
  BlendCheck has an alpha of `0.05`, so it is almost invisible, but it still
  shows the sun's highlight and the sky around its edge, which is what makes it
  look like glass. Give clear glass a low `RoughnessFactor`, such as `0.05`.
- **They cast no shadow.** A shadow map only records whether something blocks
  the light, so a pane of glass would shade the floor like a wall. In BlendCheck,
  the post casts a shadow and the panes beside it don't. See-through surfaces
  still receive shadows, and are lit by the same lights as everything else.
- **They are drawn after everything solid, farthest first.** Each one is drawn
  over whatever is behind it, so the engine sorts them by how far the centre of
  each object is from the camera. Two cases come out wrong: surfaces that pass
  through each other, like the crossed panes in BlendCheck, and one see-through
  object inside another. Split such a model into separate pieces.
- **A double-sided one shows its far side.** With `DoubleSided` on, its back
  faces are drawn first and its front faces over them, so through the glass box
  in BlendCheck you see its far walls.
- **Fog covers them as it covers anything else.** `levels/BlendFogCheck.alvl`
  shows clear and blue panes 10, 70 and 170 m away in fog, and one behind a fog
  volume.
- **They cost more than solid surfaces.** A pixel covered by three panes is
  shaded three times, and the [depth prepass](rendering-frame.md#9-depth-prepass)
  saves them no work. Keep large areas of overlapping glass to a minimum. A level
  with no see-through materials pays nothing for them.
- **A glTF material set to `BLEND` imports as `Blend`.**

## Double-sided materials

Normally, only the front of each triangle is drawn: the side that faces out of
the model. From inside a closed mesh you see nothing, which costs nothing.

`DoubleSided` (default off) draws the back of each triangle too, and lights it
as if it faced the other way. Use it for surfaces made of a single sheet, such
as a leaf, a flag or a sheet of paper, and for masked or see-through surfaces
you can see through to their back. Leave it off for closed meshes: their back faces are
never seen, and drawing them costs time.

## Shadows on smooth-shaded models

`ShadowsFollowSmoothing` (default on) keeps a smooth-shaded low-poly model from
shadowing itself in stripes where the light turns away from it. Turn it off
for foliage cards. [Smooth shading and shadows](rendering-smooth-shading.md)
explains what it does and when to change it.

## Materials from a model

When you import a model, the editor writes one `.amat` per material in it,
next to the model file, named after the model and the material: a model
`Chair.gltf` with a material called `Cushion` gets `Chair_Cushion.amat`. They
copy the model's colours, metal and roughness, textures, alpha mode and
double-sided setting, and turn `ShadowsFollowSmoothing` off for masked and
double-sided materials, which are usually foliage cards.

- **Your changes are kept.** The editor never overwrites a material file that
  exists. Edit them freely.
- **When the model changes**, for example after you export it again with a
  different material, the editor shows a list of what changed, slot by slot,
  and asks what to do: **Regenerate from source** replaces the changed
  materials with the model's new ones, losing your edits to them; **Keep my
  materials** keeps yours; **Later** asks again next time.

<details>
<summary>The other material fields</summary>

These fine-tune how light reflects. Their defaults suit almost every material.

| Field | Meaning | Default |
|---|---|---|
| `BaseWeight` | Scales the base colour: lower values darken it. | `1` |
| `SpecularWeight` | Scales the reflections on a non-metal. `0` removes them. | `1` |
| `SpecularColor` | Tints the reflections of a non-metal, and the edges of a metal. | White |
| `SpecularIor` | The **index of refraction**: how strongly a non-metal reflects, from 1 to 3. 1.5 suits most plastics, stone and paint; 1.33 is water. | `1.5` |
| `BaseDiffuseRoughness` | Above 0, makes rough non-metals look flatter and more powdery, like clay or the moon. | `0` |
| `SpecularAntiAliasing` | Softens the tiny sparkling highlights a shiny, bumpy surface can show at a distance. | On |
| `SpecularAaVarianceClamp` | The most that softening may roughen a surface, from 0 to 1. `0` turns it off. | `0.2` |

</details>
