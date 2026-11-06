# Splines

A **spline** is a curve through a list of points you place, called **nodes**.
Use one wherever something needs a route or a shape: the path a guard patrols, a
track a platform slides along, a fence line.

The engine gives you two things:

- **`Path`**, a component holding one spline, for a route on its own.
- **`Assisi::Geometry::Spline`**, the type a `Path` holds, which your own
  components can hold too.

## Try it: draw a path

### Step 1: add a Path

1. Click **+** in the **Entities** panel.
2. In the **Inspector**, add a `Transform` and then a `Path`, using the
   **Add Component** box.

### Step 2: add nodes

Hold **Alt** and click on the ground three times, in three different places. The
first click places the first node at the entity's height. Each click after that
adds a node past the end of the path, at the height of the node before it.

A curve appears through the nodes. While the entity is selected it draws in
orange, with an arrowhead on each node pointing the way the path runs. The first
node's arrowhead has a flat disc at its base and the last node's has one at its
tip, so you can see where the path starts and stops. The node you added last is
white: it is the
**shaped node**, the one the gizmo now moves. Other splines draw in magenta, or
in the colour and thickness set in the **F11** window's **Spline Color** and
**Spline Thickness**.

### Step 3: move a node

Click the middle node's arrowhead. It turns white and the move gizmo jumps to it.
Drag an arrow to move the node, and the curve follows. Press **Escape** to hand
the gizmo back to the entity.

### Step 4: change how a node shapes the curve

With the middle node still selected, look in the **Inspector**: open **Path**,
then **spline**, then **nodes**, then the middle row (`[1]`). Set its `mode` to
`Corner`. The curve becomes two straight lines meeting at a sharp corner. Set it
back to `Smooth` and the corner rounds off.

To see every mode side by side, open `levels/SplineCheck.alvl`.

## Shaping a spline in the viewport

Select the entity first: click its curve, or pick it in the **Entities** panel.

| To | Do this |
|---|---|
| Choose a node to move | Click its arrowhead. It turns white, and the gizmo moves it instead of the entity. |
| Move a Bezier node's handle | Click the small box at the handle's end, then drag the gizmo. The other handle turns with it, so the curve stays smooth. |
| Change a node's mode | Select the node, then double-click it. Each double-click steps it to the next mode: `Corner`, `Smooth`, `Bezier`, and round to `Corner`. |
| Add a node on the curve | **Alt+click** on the curve. The new node takes the mode of the node before it. Between two `Bezier` nodes the curve keeps its exact shape. |
| Add a node past an end | Select the first or last node, then **Alt+click** where the new one should go. It lands on the flat plane at that node's height; drag it up or down afterwards. |
| Add the first node | With the entity of an empty spline selected, **Alt+click** where the node should go. It lands on the flat plane at the entity's height. |
| Remove a node | Select it and press **Delete**. |
| Go back to moving the entity | Press **Escape**, or click anywhere away from the nodes. |

Hold **Ctrl** while dragging to move a node in half-metre steps. Every drag,
added node and removed node is one step of undo.

The inspector still works too: each node's `position`, `handleIn` and
`handleOut` can be typed there.

## Node modes

Each node has a `mode` that decides how the curve passes through it:

| Mode | What the curve does at the node |
|---|---|
| `Smooth` | Flows through it without a kink. The engine works out the curve from the neighbouring nodes, so you only place points. The default. |
| `Corner` | Turns sharply. Between two corner nodes the curve is a straight line. |
| `Bezier` | Follows the node's two handles, which you set yourself. |

A **handle** is an offset from its node that pulls the curve toward it.
`handleOut` shapes the curve leaving the node, and `handleIn` the curve arriving
at it. The longer a handle, the further the curve follows its direction before
turning toward the next node. Both are measured from the node's position.

Only a `Bezier` node uses the handles you type, so the inspector greys them out
for the other modes. Switching a node to `Bezier` starts its handles from the
curve already there, so nothing jumps, and you adjust from there.

Turn on `closed` to join the last node back to the first, making a loop.

## Where a spline sits

A spline's nodes are measured from its entity, the way a model's vertices are.
Moving, rotating or scaling the entity moves the whole curve with it, and a node
at `0, 0, 0` sits exactly on the entity's position.

## A spline in your own component

A component that needs a shape holds a `Spline` as a field:

```cpp
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Geometry/Spline.hpp>

/// A fence along a line the designer draws.
ACOMP()
struct Fence
{
    AFIELD() Assisi::Geometry::Spline line;
    AFIELD(min = 0) float postSpacing = 2.f;
};
```

The editor draws the spline and edits its nodes the same way it does a `Path`'s.
A component can hold any number of `Spline` fields, including inside lists.

## Example: moving along a path at a steady speed

This moves an entity along a `Path` at a set speed, looping when it reaches the
end. The moving entity is a **child** of the path's entity (its `Parent` is the
path), so its `Transform.position` is measured from the same place as the
path's nodes, and a point on the curve can be used as its position directly.

```cpp
#include <Assisi/Core/Reflect/Annotations.hpp>

/// Moves its entity along the Path on its parent.
ACOMP()
struct PathFollower
{
    AFIELD(min = 0) float metresPerSecond = 2.f;

    float travelled = 0.f; // no AFIELD: where it has got to is not saved
};
```

```cpp
#include <Assisi/App/World.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Geometry/Spline.hpp>
#include <Assisi/Runtime/Path.hpp>

#include <cmath>

// How far the chain of lines FlattenSpline makes may stray from the curve, in metres.
constexpr float kPathTolerance = 0.01f;

void FollowPathSystem(Assisi::App::SystemContext &ctx)
{
    using namespace Assisi;

    ECS::Scene &scene = ctx.world.scene;
    for (auto [entity, follower, parent, transform] :
         scene.Query<Mut<PathFollower>, ECS::Parent, Mut<ECS::Transform>>())
    {
        const Runtime::Path *path = scene.Get<Runtime::Path>(parent.parent);
        if (path == nullptr)
        {
            continue;
        }

        const Geometry::SplinePolyline chain = Geometry::FlattenSpline(path->spline, kPathTolerance);
        const float length = Geometry::PolylineLength(chain);
        if (length <= 0.f)
        {
            continue;
        }

        follower.travelled = std::fmod(follower.travelled + follower.metresPerSecond * ctx.dt, length);
        const float parameter = Geometry::ParameterAtDistance(chain, follower.travelled);
        transform.position = glm::dvec3(Geometry::SplinePoint(path->spline, parameter));
    }
}
```

Declare the system with `ASYSTEM` and add it to the level's systems, as in
[Your first system](first-system.md).

Two things in this example need explaining:

- **Parameters.** A position along a spline is a number called its
  *parameter*: `0` at the first node, `1` at the second, `2` at the third, and so
  on. A parameter of `1.5` is part-way between the second and third nodes, but
  not necessarily halfway along that stretch, because the curve can move faster
  in some places than others.
- **Steady speed.** `FlattenSpline` turns the curve into a chain of short
  straight lines and records how far along the curve each one is.
  `ParameterAtDistance` then turns "this many metres along" into a parameter, so
  equal steps of distance are equal steps along the curve.

The example flattens the spline every frame to stay short. For a long path, keep
the chain and flatten again only when the spline changes.

## Spline functions

All in `<Assisi/Geometry/Spline.hpp>`, in namespace `Assisi::Geometry`.

| Function | What it gives you |
|---|---|
| `SplinePoint(spline, parameter)` | The point at a parameter. |
| `SplineDirection(spline, parameter)` | The direction of travel there, of length 1. |
| `SegmentCount(spline)` | How many stretches the curve has between nodes, which is also its last parameter. |
| `FlattenSpline(spline, tolerance)` | The curve as a chain of straight lines, none further than `tolerance` from it. |
| `PolylineLength(chain)` | The curve's length. |
| `ParameterAtDistance(chain, distance)` | The parameter a distance along the curve. |
| `ClosestOnSpline(spline, chain, point)` | The point on the curve nearest another point, with its parameter and distance along. |
| `SettleHandles(spline)` | Stores in each `Smooth` and `Corner` node the handles it draws with. The editor does this after every edit; call it after changing nodes in code if you might switch them to `Bezier` later. |

Every function works in the spline's own space, measured from its entity.
