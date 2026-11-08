# Wind

A level's wind comes from one `Wind` component, in
`<Assisi/Runtime/WindComponents.hpp>`. The clouds drift with it, and the sea
and lakes grow waves from it. Put it on any entity. A level without one has
still air: the clouds hang where they are, and the water is flat apart from
an ocean's swell.

## Try it: change the wind

### Step 1: Give the clouds a wind

Open the editor on `levels/CloudCheck.alvl`. Its clouds don't move, because
the level has no wind.

1. Click **+** in the **Entities** panel. You get an empty entity, selected.
2. In the **Inspector**, type `Wind` in the **Add Component** box and press
   **Enter**. The clouds start to drift east, and their shadows cross the plain
   with them.
3. Set `bearingDegrees` to `0`. The clouds turn and drift north.

### Step 2: Change the sea's wind

Open `levels/WaterCheck.alvl`. Select the `Wind` entity: `speed` is 10.

1. Set `speed` to `20`, a gale. The sea's waves grow taller and longer, and
   their crests sharpen. The lake's waves grow too, but stay small.
2. Set `speed` to `0`. Only the long swell still rolls through the sea, and the
   lake goes flat.

## `Wind` fields

| Field | Meaning |
|---|---|
| `speed` | The wind's speed in metres per second, from `0` to `40`. A breeze is about 5, a gale about 20. |
| `bearingDegrees` | The compass bearing the wind blows **toward**: `0` is north, `90` east. |

`speed` means the same thing as a weather forecast's wind speed: the average
over ten minutes, 10 metres above the ground. A forecast gives the bearing the
wind blows **from**, so add or take away 180 to use one: a wind "from the
west" blows toward `90`.

## Who reads the wind

- **Clouds** drift with it, half as fast again as the wind. Near the ground,
  friction slows the wind; up where the clouds are, it blows 1.25 to 2 times
  as fast. See [Sky and time of day](rendering-sky.md#clouds).
- **Water** grows waves from it. The waves on an `Ocean` and a `Lake` run the
  way the wind blows, and grow with its speed and with how far it has blown
  over the water. An ocean's swell is its own: it comes from a storm far away.
  See [Water](rendering-water.md).

## Limits

- **One wind for the whole level.** A sheltered valley gets the same wind as
  an open ridge. With two `Wind` components, the first found is used and the
  log says so.
- **The wind is steady.** It has no gusts yet.
- **Changing the wind during play makes the sea's waves jump.** How big the
  wave patterns are depends on the wind, so a new speed lays them out again.
  Set the wind when the level loads.
- **Only the clouds and the water read it so far.**
