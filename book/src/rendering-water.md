# Water

A level gets water from two components, in `<Assisi/Runtime/WaterComponents.hpp>`:

| Component | What it is |
|---|---|
| `Ocean` | Endless water at its entity's height, out to the horizon: the sea. |
| `Lake` | Water inside a rectangle at its entity's height, turned and scaled with the entity: a lake, or a pond. |

Both draw real waves that move, catch the sun, reflect the sky, and show the
ground through them where the water is shallow. A level without either pays
nothing for water.

## Try it: an ocean and a lake

### Step 1: Open the water level

Open the editor on `levels/WaterCheck.alvl`. It has a sea, a beach sloping
into it on the left, and a stone basin on the right with a lake on top.

1. Look along the beach. Where it goes under the water, the sand shows through
   in the water's colour, and deeper out it fades into dark blue. There is no
   hard line where the water meets the beach.
2. Look toward the sun. The waves throw a path of sunlight back at you.

### Step 2: Change the wind

1. Select the `Sea` entity. Its `Ocean` has `windSpeed` 10.
2. Set `windSpeed` to `20`, a gale. The waves grow taller and longer, and
   their crests sharpen.
3. Set it to `2`. The sea goes almost calm, and only the long swell is left
   rolling through it.
4. Set `swellHeight` to `0` too. Now the sea is nearly flat.

### Step 3: Look at the lake

1. Fly up over the basin. The lake has small waves of its own. Set its
   `windSpeed` to 10, the sea's: they stay far smaller than the sea's, because
   wind blowing over only 30 metres of water can't build big waves.
2. Select the `Lake` entity and set `clarity` to `0.5`. The water turns murky,
   and the floor of the basin disappears.

## How the waves are made

The surface is the sum of thousands of waves of every length and direction at
once. How big the waves of each length are comes from measurements of real
seas, for the wind you set. Each frame the graphics card moves every wave on to
the current time and adds them all up.

Three things decide the waves:

- **The wind:** its speed, and the compass bearing it blows toward. Most waves
  travel the way the wind blows; `spread` lets some travel off to the sides.
- **Fetch:** how far the wind has blown over open water. A long fetch grows
  long, tall waves. An `Ocean`'s fetch is the open sea's. A `Lake`'s is its own
  width along the wind, which is why a pond only ripples.
- **Swell:** long, even waves from a storm far away, which an `Ocean` adds to
  its own wind's waves. You set it as a forecast gives it: a height and the
  seconds between crests.

`choppiness` pulls the surface sideways toward each crest, which makes crests
sharp and troughs broad, the way real waves look. At `0` the surface only moves
up and down, and the waves look rounded, like jelly.

Long waves travel faster than short ones, as at sea: a swell 200 metres long
moves at about 18 metres a second, a ripple 10 centimetres long at under half a
metre a second.

## `Ocean` fields

| Field | Meaning |
|---|---|
| `windSpeed` | The local wind, in metres per second. A breeze is about 5, a gale about 20. |
| `windBearingDegrees` | The compass bearing the wind blows toward: `0` is north, `90` east. |
| `spread` | From `0`, waves travel close to the wind, to `1`, waves travel every way equally. |
| `swellHeight` | The swell's height in metres: about the height of its bigger waves from trough to crest. `0` is no swell. |
| `swellPeriod` | Seconds between the swell's crests, from 4 to 25. Longer swell is longer and faster, and has come further. |
| `swellBearingDegrees` | The compass bearing the swell travels toward. It need not match the wind. |
| `choppiness` | How sharp the crests are, from `0` to `2`. Past about `1.5` the steepest crests fold over themselves. |
| `color` | The colour that comes back out of deep water. |
| `crestColor` | The colour of light that has crossed only a little water. Crests glow with it when the sun is behind them, and shallow water takes its hue. |
| `clarity` | How far you can see into the water, in metres: light this deep has lost all but a third of itself. Shallower than this, the ground shows through. |
| `seed` | Which sea of its kind. The same settings with another seed give other waves. |

## `Lake` fields

A `Lake` has the same `windSpeed`, `windBearingDegrees`, `spread`,
`choppiness`, `color`, `crestColor`, `clarity` and `seed` as an `Ocean`, and no
swell. It adds:

| Field | Meaning |
|---|---|
| `size` | Its width along the entity's x and z, in metres. Scaling the entity scales it. |

Put a `Lake` where ground surrounds it at or above its height, so that the
ground hides its edges.

## The water's colour

Looking down into water, you see three things mixed together:

1. **The sky reflected in the waves.** Looking straight down you see little of
   it; toward the horizon, almost nothing else. This is why far water looks
   like the sky.
2. **Light scattered back out of the water.** This is `color` in deep water.
   Where a crest stands between you and a low sun, light shines through it in
   `crestColor`.
3. **The ground under the water**, fading with depth. `clarity` sets how
   quickly. Red fades first, faster for water whose `crestColor` has little
   red, so shallows turn blue-green, and a lake with a green `crestColor`
   turns green.

The sun's highlight and the reflection come from how the waves tilt within each
pixel. Far away, a pixel covers many small waves, so the water there looks
rough and glossy rather than glittering.

## Water and multiplayer

The waves depend only on the components' fields and the time. Every machine in
a session runs the same clock, so every player sees the same waves at the same
moment, and nothing about them is sent over the network.

## Limits

- **The camera must stay above the water.** From below, the surface is not
  drawn.
- **A lake is a rectangle.** Where the ground outside it drops below its
  height, the rectangle's edge shows.
- **The waves don't feel the ground yet.** They are as tall over a beach as in
  deep water, and don't break on the shore.
- **There is no foam.**
- **Nothing floats.** Bodies fall through the water.

## What water costs

Water adds two passes to the frame (see
[How a frame is drawn](rendering-frame.md)):

- **`water-waves`** moves each body in view on to the time: four 256 by 256
  transforms and their smaller copies. It costs the same whatever the screen's
  size, once for each body in view. A body out of view costs nothing.
- **`draw-water`** draws the surface. Its cost grows with the screen area the
  water covers.

Water also needs the scene's depth as a distance, which ambient occlusion
already makes; with ambient occlusion off, water makes it itself.

A player can be allowed to turn water off; see
[Water](rendering-settings.md#water) in the graphics settings.
