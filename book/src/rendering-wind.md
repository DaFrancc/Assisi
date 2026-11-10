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
   their crests sharpen. Ripples run across the lake with the wind.
2. Set `speed` to `4`. The lake goes calm and only rocks on its swell: its
   banks keep half the wind off, and even the strongest gusts leave too little
   at the water to raise ripples.
3. Set `speed` to `0`. Only the long swell still rolls through the sea, and the
   lake only rocks.

## `Wind` fields

| Field | Meaning |
|---|---|
| `speed` | The wind's speed in metres per second, from `0` to `40`. A breeze is about 5, a gale about 20. |
| `bearingDegrees` | The compass bearing the wind blows **toward**: `0` is north, `90` east. |
| `gustFactor` | How much stronger the strongest gusts are than `speed`, from `1` to `3`. The default `1.5` gusts to half as fast again and lulls to half as fast; `1` is a steady wind. |

`speed` means the same thing as a weather forecast's wind speed: the average
over ten minutes, 10 metres above the ground. A forecast gives the bearing the
wind blows **from**, so add or take away 180 to use one: a wind "from the
west" blows toward `90`.

## Gusts

The wind is not steady: patches of stronger and weaker wind, about 60 metres
long and half as wide, drift across the level with it and change as they go.
Every machine in a session sees the same gusts at the same moment, so nothing
about them is sent over the network.

Ripples on water follow the gusts: in a light wind, they appear only where a
gust is strong enough to raise them. The clouds keep to the steady wind.

## Who reads the wind

- **Clouds** drift with it, half as fast again as the wind. Near the ground,
  friction slows the wind; up where the clouds are, it blows 1.25 to 2 times
  as fast. See [Sky and time of day](rendering-sky.md#clouds).
- **Water** grows waves from it. The waves on an `Ocean` and a `Lake` run the
  way the wind blows, and grow with its speed and with how far it has blown
  over the water. Ripples come and go with the gusts. An ocean's swell, and a
  lake's gentle swell, are their own. See [Water](rendering-water.md).

## Limits

- **One wind for the whole level.** A sheltered valley gets the same wind as
  an open ridge. With two `Wind` components, the first found is used and the
  log says so.
- **The gusts are the same size everywhere.** They don't change behind a hill
  or a wood.
- **Changing the wind during play makes the sea's waves jump.** How big the
  wave patterns are depends on the wind, so a new speed lays them out again.
  Set the wind when the level loads.
- **Only the clouds and the water read it so far.**
