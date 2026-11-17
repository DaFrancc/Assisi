# Lights

A surface is lit by the light components in the level, plus a weak light from
all around called **ambient** or **indirect** light. This page covers the light
components. Ambient light comes from the sky, if the level has one; see
[The light the sky casts](rendering-sky.md#the-light-the-sky-casts).

All three light components are in `<Assisi/Runtime/LightComponents.hpp>`:

| Component | Use it for | Needs a `Transform` |
|---|---|---|
| `DirectionalLight` | The sun or the moon: light from far away that arrives from one direction everywhere in the level. | No. It has no position. |
| `PointLight` | A bulb, a candle, a fire: light that spreads out in every direction from one point. | Yes, for its position. |
| `SpotLight` | A torch, a stage light, a car's headlight: light from one point, in a cone. | Yes, for its position and its aim. |

Every light casts shadows unless you turn its `castsShadows` off. Shadows cost
more than the light itself; see [Shadows](rendering-shadows.md).

## Try it: a lamp and a torch

Open the editor on the test level:

```bash
./out/build/gcc-dev/apps/game/Assisi-GameEditor -l levels/Test.alvl
```

### Step 1: Add a lamp

1. Click **+** in the **Entities** panel and add a `Transform`. The entity is
   placed 5 m in front of the camera.
2. Add a `PointLight`.
3. Move it with the gizmo until it is about 2 m above the floor.
4. Set `intensity` to `20` and `radius` to `8`.

A pool of light appears on the floor around it, and the objects near it cast
shadows away from it. The editor draws the light as a faint sphere the size of
its `radius`: nothing outside the sphere is lit by it.

### Step 2: Add a torch

1. Add another entity with a `Transform` and a `SpotLight`.
2. Set `intensity` to `40` and `radius` to `15`.
3. Press **E** for the rotate gizmo and turn the entity until the cone the
   editor draws for it points at the floor.

A spot light shines down its entity's local −Y axis, so turning the entity is
how you aim it.

### Step 3: Turn off a shadow

Untick the lamp's `castsShadows`. Its light now passes through the objects
around it, and their shadows on the floor disappear.

## `DirectionalLight`

| Field | Meaning |
|---|---|
| `direction` | The direction the light travels, as a vector. The default, `(0, -1, 0)`, shines straight down. Ignored when the entity also has a `Sun`, which aims it from the time of day instead. |
| `intensity` | How bright it is. Default 1. |
| `castsShadows` | Whether it casts shadows. |
| `tintedBySky` | On (the default), and with a `Skybox` on the same entity, the light takes its colour from the sky: white at noon, orange and dimmer at sunset. Without a `Skybox` it does nothing, and the light is white unless you set a colour. |
| `colorExpression` | How you give the colour: `Rgb` uses `color`, `Temperature` uses `temperatureKelvin`. |
| `color` | The colour, as red, green and blue from 0 to 1. |
| `temperatureKelvin` | The colour as a temperature, the way real light is described. Low is warm: about 2800 K for a household bulb, 5800 K for midday sun, 6500 K for an overcast sky, above 10000 K for a blue sky. |

The inspector hides the three colour fields while `tintedBySky` is ticked. To
give a light a colour of its own, untick it first.

A level can have more than one directional light, and they all light the
scene, but:

- **Only one of them casts shadows**: the first the engine finds with
  `castsShadows` on.
- **A sky needs exactly one.** With two or more, the sky is not drawn, a `Sun`
  no longer moves its light, and the log says so once.

A `DirectionalLight` also carries the sky, the sun's movement and the moon when
you add those components beside it. See [Sky and time of day](rendering-sky.md).

## `PointLight`

| Field | Meaning |
|---|---|
| `color` | The colour, as red, green and blue from 0 to 1. |
| `intensity` | How bright it is. A negative value takes light away, which can darken a corner. |
| `radius` | How far the light reaches, in metres. Nothing beyond it is lit. |
| `castsShadows` | Whether it casts shadows. A point light's shadow costs six times a spot light's; see [Shadows](rendering-shadows.md#spot-and-point-light-shadows). |
| `shadowPriority` | Which lights keep their shadows when there are more shadowed lights than the game allows. See [Which lights get a shadow](rendering-shadows.md#which-lights-get-a-shadow). |
| `shadowAlwaysOn` | This light always keeps its shadow when lights compete for one. |

The light is brightest at the entity's position and fades with the square of
the distance, reaching exactly zero at `radius`. With a `radius` well beyond
them, a surface 1 m away gets about half of `intensity`, and one 4 m away about
a seventeenth.

## `SpotLight`

A spot light has all of a point light's fields, plus the size of its cone:

| Field | Meaning |
|---|---|
| `innerAngle` | Inside this angle from the centre of the cone, in degrees, the light is at full brightness. It can't be larger than `outerAngle`. |
| `outerAngle` | The edge of the cone, in degrees from its centre, up to 89. Between `innerAngle` and `outerAngle` the light fades out. |

The angles are measured from the centre line, so an `outerAngle` of 30 makes a
cone 60° across. A small gap between the two angles gives a hard-edged circle of
light; a wide gap gives a soft one.

## How bright to make a light

`intensity` is a multiplier, not a real-world unit such as lumens. As a guide,
the levels in `assets/levels/` use:

| Light | Typical `intensity` |
|---|---|
| A `DirectionalLight` sun | 0.25 to 2.5 |
| A `PointLight` lighting a few metres around it | 10 to 60 |
| A `SpotLight` | 20 to 60 |

The renderer adds up light without any upper limit, then the **tone map** turns
the result into colours a screen can show, so a very bright light looks bright
rather than flat white. The player can change the tone map's brightness; see
[Tone mapping](rendering-settings.md#tone-mapping).

## What lights cost

How a light is calculated depends on its type.

- **Every directional light is calculated for every pixel** that shows a
  surface. Each one adds the same cost whatever it lights. Keep to one or two.
- **Point and spot lights are calculated only where they reach.** The engine
  divides the camera's view into a grid of 16 × 9 × 24 boxes called
  **clusters**: 16 across, 9 down, and 24 slices of distance. Each frame it
  lists which lights reach into each cluster, and each pixel calculates only the
  lights in its own cluster. A light costs roughly in proportion to how much of
  the screen its `radius` covers, so a small radius is cheaper than a large one,
  and a light whose reach is entirely off screen adds no work to any pixel.

So many small lights are fine, but many large lights overlapping the same part
of the screen make every pixel there slower. A light's shadow costs more than
its light: keep `castsShadows` on only where the shadow matters.

The engine draws at most 1024 point lights, 1024 spot lights and 16 directional
lights at once. Lights past those limits are not drawn, and the log warns once.

<details>
<summary>Lights in multiplayer</summary>

Light components are not sent over the network. Each machine draws the lights
the level file gives it. If your code changes a light while the game runs, such
as a lamp that flickers, it changes it only on the machine the code runs on.

</details>
