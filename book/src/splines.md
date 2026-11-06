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

1. In the `Path` component, open **spline**, then **nodes**.
2. Press **+ Add** three times.
3. Open each row and type its `position`: `0, 0, 0`, then `5, 0, 0`, then
   `5, 0, 5`.

A curve appears through the three nodes. While the entity is selected it draws
in orange, with a box on each node; the first node's box is larger, so you can
see which end the path starts from. Other splines draw in magenta.

### Step 3: change how a node shapes the curve

Set the middle node's `mode` to `Corner`. The curve becomes two straight lines
meeting at a sharp corner. Set it back to `Smooth` and the corner rounds off.

To see every mode side by side, open `levels/SplineCheck.alvl`.

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
