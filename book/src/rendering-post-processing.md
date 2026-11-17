# The level's look

After a frame is drawn, the engine finishes it: it sets how bright the picture
is, balances its colours, adds contrast and saturation, and lets bright light
glow. This is called **post-processing**. A level sets its own finishing with
the `PostProcessVolume` component, so a sunny valley and a dark cave can each
look the way you want.

The player's graphics settings go on top: a brightness slider, the tone curve,
and whether bloom is on at all. See [Graphics settings](rendering-settings.md).

## The PostProcessVolume component

A `PostProcessVolume` applies either to the whole level or to a box:

- **`extent` set to `Level`** is the level's own look, everywhere. Put one in
  each level that needs a look of its own.
- **`extent` set to `Box`** changes the look inside a box centred on its
  entity, such as a cave or a room. The box is `size` metres wide, high and
  deep, and turns and scales with the entity. Outside the box, the volume fades
  in over `blendDistance` metres as the camera nears it, so the look never
  jumps at the edge. The editor draws the box, and faintly, the edge where the
  fade starts.

Each setting has an **override** switch beside it, off at first. A volume
changes only the settings you switch on and leaves the rest alone. A cave
volume that only switches on **Override Exposure** darkens the picture and
keeps the level's contrast, saturation and bloom.

A level with no volume looks the way it did before volumes existed.

## What a volume can set

| Override | Fields | What it does |
|---|---|---|
| **Exposure** | `exposureMode` and the fields below | How bright the picture is drawn. |
| **Contrast** | `contrast`, 0.5 to 2.5 | Darkens the shadows against the highlights. 1 changes nothing; the default is 1.35. |
| **Saturation** | `saturation`, 0 to 2 | How strong colours are. 0 is black and white, 1 changes nothing; the default is 1.4. |
| **White balance** | `whiteBalanceKelvin`, `whiteBalanceTint` | Which colour of light comes out white. |
| **Bloom** | `bloomIntensity`, `bloomScatter` | How strong the glow around bright light is, and how widely it spreads. |

### Exposure

**Exposure** is how bright the whole picture is drawn, in **stops**: each stop
doubles or halves it. With `exposureMode` set to:

- **`Manual`**, the picture is drawn `exposureStops` brighter, or darker for a
  negative number. 0 draws the scene's light as it is.
- **`Automatic`**, the exposure follows the scene, the way eyes adjust: it
  brightens in a dark room and darkens back in daylight, over a second or two.
  A daylit scene comes out about as it does with no exposure at all.

The automatic exposure has four more fields:

| Field | What it does | Default |
|---|---|---|
| `minExposureStops` | The darkest it ever draws, in stops. | −4 |
| `maxExposureStops` | The brightest it ever draws. Keep this low in a cave you want to stay dark. | 8 |
| `exposureCompensation` | Stops added to what it aims for: positive draws everything brighter. | 0 |
| `adaptationSpeed` | How fast it adjusts. At 1.5, about two thirds of a change happens in the first two thirds of a second. | 1.5 |

The automatic exposure measures the brighter half of the picture, so a dark
corner on screen doesn't brighten everything, and it ignores the brightest few
pixels, so looking at the sun doesn't darken everything.

### White balance

`whiteBalanceKelvin` is the colour of light that comes out white, as a
temperature in kelvin. Light sources are described the same way: about 2700 K
for a household bulb, 3200 K for a stage lamp, 5500 K for noon sun and 6500 K
for an overcast sky.

- **Set it to the light in the scene** to make that light look white. In a room
  lit by 3000 K lamps, 3000 K turns the lamplight neutral; any daylight in the
  room then looks blue.
- **Higher than 6500** warms the whole picture; **lower** cools it.
- **`whiteBalanceTint`** shifts it the other way, from −1, toward green, to 1,
  toward magenta.

6500 K and no tint change nothing.

### Bloom

`bloomIntensity` is the share of the picture the glow replaces, 0 to 0.5. The
default, 0.05, is a modest glow. `bloomScatter`, 0 to 0.95, sets how widely it
spreads: low keeps it tight around each light, high spreads a faint haze. The
player can turn bloom off; see [Bloom](rendering-settings.md#bloom).

## When volumes overlap

The camera's position decides which volumes count:

1. The whole-level volume goes first.
2. Each box the camera is in, or near enough to be fading in, goes over it.
3. Of two boxes, the one with the higher `priority` goes on top. At equal
   priority, the smaller box goes on top, so a room inside a cave inside a
   valley works without setting any priorities.
4. The player's brightness goes on last.

Two whole-level volumes in one level is a mistake: the one with the higher
`priority` wins, and the log says so once.

## Example: zones in a yard, and a dark hut

A box doesn't have to be indoors: it is only a region of space. While the
camera is inside it, or within its blend distance, its look applies to the
whole picture. From outside a box you see nothing of it, because it changes the
picture, not the objects inside it.

`levels/PostProcessCheck.alvl` has a daylit yard, four spheres, three outdoor
zones and a closed hut lit by one warm lamp:

- **LevelLook**, `extent` `Level`, switches on **Override Exposure** with
  `exposureMode` `Automatic`. In the yard, the picture looks as it would with
  no exposure.
- **NoirZone**, a box beside the spheres, switches on **Override Saturation**
  at 0 and **Override Contrast** at 1.9: black and white, with deep shadows.
- **GoldenZone**, a box on the other side, sets 16000 K white balance with a
  little magenta tint, saturation 1.8, contrast 1.5, and strong bloom. Walk
  from between the two zones toward it and the picture turns warm over its 3 m
  `blendDistance`.
- **FrostZone**, behind the spheres, sets 3200 K, which cools everything, with
  saturation 0.6 and automatic exposure brightened by 0.8 stops.
- **HutLook** fills the hut with a 1.5 m `blendDistance`. Its automatic
  exposure stops at `maxExposureStops` 3, so the hut brightens as you walk in
  but stays dim. It warms the picture to 11000 K and adds saturation and
  contrast.

## What it costs

Contrast, saturation and white balance cost nothing; they are part of the tone
map, which runs every frame. Automatic exposure adds two small passes, one
reading the picture at half its width and height and one tiny one, but only
while some of the exposure is automatic. For bloom, see [Bloom](rendering-settings.md#bloom).
