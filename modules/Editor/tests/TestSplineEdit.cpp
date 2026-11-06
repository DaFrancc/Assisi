/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestSplineEdit.cpp
/// @brief Shaping a spline in the viewport: a click finds the node, handle or
///        stretch of curve under it; inserting and extending add the nodes
///        expected; dragging a handle keeps its partner in line; and each edit
///        undoes in one step.
///
/// The camera is the one TestOutlinePicking uses: at the origin, looking down
/// -Z, 90 degrees across a square viewport, so a point's screen position can be
/// worked out by hand.

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <optional>

#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Editor/EditHistory.hpp>
#include <Assisi/Editor/SplineEdit.hpp>
#include <Assisi/Geometry/Spline.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/Path.hpp>

#include <glm/gtc/matrix_transform.hpp>

using namespace Assisi;
using Editor::PickRay;
using Editor::SplineCurveHit;
using Editor::SplinePart;
using Editor::SplinePartHit;
using Editor::SplinePartKind;
using Geometry::Spline;
using Geometry::SplineNode;
using Geometry::SplineNodeMode;

namespace
{

constexpr float kViewport = 800.f;

/// Room for float rounding in positions of a few metres.
constexpr float kPositionTolerance = 1e-4f;

/// Further than any pick tolerance from the thing aimed at, in pixels.
constexpr float kWellAway = 40.f;

/// On the marker, as far as a cursor can tell, in pixels.
constexpr float kSubPixel = 0.01f;

/// Comfortably inside a marker's pick tolerance, in pixels.
constexpr float kSlightlyOff = 3.f;

PickRay Camera()
{
    PickRay ray;
    ray.origin = glm::vec3(0.f);
    ray.direction = glm::vec3(0.f, 0.f, -1.f);
    ray.viewProjection = glm::perspective(glm::radians(90.f), 1.f, 0.1f, 1000.f);
    ray.viewportSize = glm::vec2(kViewport);
    ray.valid = true;
    return ray;
}

/// The camera's ray through @p world, as the editor builds one through the
/// cursor over that point.
PickRay RayThrough(glm::vec3 world)
{
    PickRay ray = Camera();
    ray.direction = glm::normalize(world);
    return ray;
}

glm::vec2 Screen(const PickRay &ray, glm::vec3 world)
{
    const glm::vec4 clip = ray.viewProjection * glm::vec4(world, 1.f);
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    return glm::vec2((ndc.x * 0.5f + 0.5f) * ray.viewportSize.x, (0.5f - ndc.y * 0.5f) * ray.viewportSize.y);
}

void CheckNear(const glm::vec3 &actual, const glm::vec3 &expected)
{
    CHECK(glm::length(actual - expected) <= kPositionTolerance);
}

/// Three nodes ten metres out, across the middle of the view.
Spline Arch(SplineNodeMode mode)
{
    Spline spline;
    spline.nodes = {SplineNode{.position = {-2.f, 0.f, -10.f}, .mode = mode},
                    SplineNode{.position = {0.f, 1.f, -10.f}, .mode = mode},
                    SplineNode{.position = {2.f, 0.f, -10.f}, .mode = mode}};
    Geometry::SettleHandles(spline);
    return spline;
}

} // namespace

TEST_CASE("SplineEdit: a click on a node's marker picks the node")
{
    const PickRay ray = Camera();
    const Spline spline = Arch(SplineNodeMode::Smooth);
    const glm::mat4 identity(1.f);

    const std::optional<SplinePartHit> exact =
        PickSplinePart(ray, Screen(ray, spline.nodes[1].position), identity, spline);
    REQUIRE(exact.has_value());
    CHECK(exact->part == SplinePart{.node = 1, .kind = SplinePartKind::Node});
    CHECK(exact->pixels < kSubPixel);

    const glm::vec2 nearby = Screen(ray, spline.nodes[2].position) + glm::vec2(kSlightlyOff, -kSlightlyOff);
    const std::optional<SplinePartHit> close = PickSplinePart(ray, nearby, identity, spline);
    REQUIRE(close.has_value());
    CHECK(close->part.node == 2);

    const glm::vec2 away = Screen(ray, spline.nodes[1].position) + glm::vec2(0.f, kWellAway);
    CHECK_FALSE(PickSplinePart(ray, away, identity, spline).has_value());
}

TEST_CASE("SplineEdit: the start, the end and every node between have their own markers")
{
    Spline spline = Arch(SplineNodeMode::Smooth);
    CHECK(Editor::MarkerOf(spline, SplinePart{.node = 0}) == Editor::SplineMarker::Start);
    CHECK(Editor::MarkerOf(spline, SplinePart{.node = 1}) == Editor::SplineMarker::Middle);
    CHECK(Editor::MarkerOf(spline, SplinePart{.node = 2}) == Editor::SplineMarker::End);
    CHECK(Editor::MarkerOf(spline, SplinePart{.node = 1, .kind = SplinePartKind::HandleOut}) ==
          Editor::SplineMarker::Handle);

    // A loop runs on through its last node, so it has no end.
    spline.closed = true;
    CHECK(Editor::MarkerOf(spline, SplinePart{.node = 2}) == Editor::SplineMarker::Middle);
}

TEST_CASE("SplineEdit: a click anywhere on a nearby node's marker picks it, not only near its centre")
{
    // Two metres out, where a marker spans far more than the pixel tolerance.
    Spline spline;
    spline.nodes = {SplineNode{.position = {-1.f, 0.f, -2.f}}, SplineNode{.position = {0.f, 0.f, -2.f}},
                    SplineNode{.position = {1.f, 0.f, -2.f}}};
    const glm::mat4 identity(1.f);

    // Inside the middle node's arrowhead, but well past the pixel tolerance.
    const glm::vec3 onArrowhead = spline.nodes[1].position + glm::vec3(0.f, Editor::kArrowheadHalfLength * 0.8f, 0.f);
    const PickRay onRay = RayThrough(onArrowhead);
    REQUIRE(glm::length(Screen(onRay, onArrowhead) - Screen(onRay, spline.nodes[1].position)) >
            Editor::kSplinePartPickPixels);
    const std::optional<SplinePartHit> hit = PickSplinePart(onRay, Screen(onRay, onArrowhead), identity, spline);
    REQUIRE(hit.has_value());
    CHECK(hit->part.node == 1);

    // As far out from the middle node as an end's disc reaches: past its arrowhead.
    const glm::vec3 discReach = glm::vec3(0.f, Editor::kEndDiscRadius * 0.9f, 0.f);
    const PickRay pastMiddle = RayThrough(spline.nodes[1].position + discReach);
    CHECK_FALSE(PickSplinePart(pastMiddle, Screen(pastMiddle, spline.nodes[1].position + discReach), identity, spline)
                .has_value());

    // The start's disc reaches that far, so it is picked there.
    const PickRay onDisc = RayThrough(spline.nodes[0].position + discReach);
    const std::optional<SplinePartHit> start =
        PickSplinePart(onDisc, Screen(onDisc, spline.nodes[0].position + discReach), identity, spline);
    REQUIRE(start.has_value());
    CHECK(start->part.node == 0);
}

TEST_CASE("SplineEdit: a node is picked where its entity puts it")
{
    const PickRay ray = Camera();
    const Spline spline = Arch(SplineNodeMode::Corner);
    const glm::mat4 moved = glm::translate(glm::mat4(1.f), glm::vec3(3.f, 0.f, 0.f));

    const std::optional<SplinePartHit> picked =
        PickSplinePart(ray, Screen(ray, glm::vec3(1.f, 0.f, -10.f)), moved, spline); // node 0, moved
    REQUIRE(picked.has_value());
    CHECK(picked->part.node == 0);
    CHECK_FALSE(PickSplinePart(ray, Screen(ray, spline.nodes[0].position), moved, spline).has_value());
}

TEST_CASE("SplineEdit: only a Bezier node offers its handles")
{
    const PickRay ray = Camera();
    const glm::mat4 identity(1.f);

    Spline spline = Arch(SplineNodeMode::Smooth);
    const glm::vec3 smoothTip = spline.nodes[1].position + spline.nodes[1].handleOut;
    CHECK_FALSE(PickSplinePart(ray, Screen(ray, smoothTip), identity, spline).has_value());

    spline.nodes[1].mode = SplineNodeMode::Bezier;
    spline.nodes[1].handleIn = {-1.5f, 0.f, 0.f};
    spline.nodes[1].handleOut = {1.5f, 0.f, 0.f};
    const std::optional<SplinePartHit> out =
        PickSplinePart(ray, Screen(ray, spline.nodes[1].position + spline.nodes[1].handleOut), identity, spline);
    REQUIRE(out.has_value());
    CHECK(out->part == SplinePart{.node = 1, .kind = SplinePartKind::HandleOut});
    const std::optional<SplinePartHit> in =
        PickSplinePart(ray, Screen(ray, spline.nodes[1].position + spline.nodes[1].handleIn), identity, spline);
    REQUIRE(in.has_value());
    CHECK(in->part == SplinePart{.node = 1, .kind = SplinePartKind::HandleIn});
}

TEST_CASE("SplineEdit: a click on the curve finds where along it, corrected for perspective")
{
    const PickRay ray = Camera();
    // A straight stretch running away from the eye, so the near quarter of it
    // takes far more of the screen than a quarter.
    Spline line;
    line.nodes = {SplineNode{.position = {-4.f, -1.f, -5.f}, .mode = SplineNodeMode::Corner},
                  SplineNode{.position = {4.f, -1.f, -45.f}, .mode = SplineNodeMode::Corner}};
    const glm::vec3 quarter = glm::mix(line.nodes[0].position, line.nodes[1].position, 0.25f);

    const std::optional<SplineCurveHit> hit = PickSplineCurve(ray, Screen(ray, quarter), glm::mat4(1.f), line);
    REQUIRE(hit.has_value());
    CHECK(hit->parameter == doctest::Approx(0.25f).epsilon(1e-3));
    CHECK(hit->distance == doctest::Approx(glm::length(quarter)).epsilon(1e-3));

    CHECK_FALSE(
        PickSplineCurve(ray, Screen(ray, quarter) + glm::vec2(0.f, kWellAway), glm::mat4(1.f), line).has_value());
}

TEST_CASE("SplineEdit: inserting between two Bezier nodes keeps the curve's shape")
{
    Spline spline;
    spline.nodes = {SplineNode{.position = {0.f, 0.f, 0.f}, .handleOut = {1.f, 2.f, 0.f}, .mode = SplineNodeMode::Bezier},
                    SplineNode{.position = {6.f, 0.f, 1.f}, .handleIn = {-2.f, -1.f, 1.f}, .mode = SplineNodeMode::Bezier}};
    const Spline before = spline;
    constexpr float kSplit = 0.3f;

    const std::size_t index = Editor::InsertSplineNode(spline, kSplit);
    CHECK(index == 1);
    REQUIRE(spline.nodes.size() == 3);
    CHECK(spline.nodes[1].mode == SplineNodeMode::Bezier);
    CheckNear(spline.nodes[1].position, Geometry::SplinePoint(before, kSplit));

    for (const float u : {0.1f, 0.2f, 0.5f, 0.8f, 0.95f})
    {
        const float after = u <= kSplit ? u / kSplit : 1.f + (u - kSplit) / (1.f - kSplit);
        CheckNear(Geometry::SplinePoint(spline, after), Geometry::SplinePoint(before, u));
    }
}

TEST_CASE("SplineEdit: inserting on a closed spline's last stretch adds the node at the end")
{
    Spline loop;
    loop.closed = true;
    loop.nodes = {SplineNode{.position = {0.f, 0.f, 0.f}}, SplineNode{.position = {4.f, 0.f, 0.f}},
                  SplineNode{.position = {4.f, 0.f, 4.f}}, SplineNode{.position = {0.f, 0.f, 4.f}}};
    const glm::vec3 onCurve = Geometry::SplinePoint(loop, 3.5f);

    const std::size_t index = Editor::InsertSplineNode(loop, 3.5f);
    CHECK(index == 4);
    REQUIRE(loop.nodes.size() == 5);
    CHECK(loop.nodes[4].mode == SplineNodeMode::Smooth);
    CheckNear(loop.nodes[4].position, onCurve);
}

TEST_CASE("SplineEdit: extending adds a node past either end, in that end's mode")
{
    Spline spline;
    spline.nodes = {SplineNode{.position = {0.f, 0.f, 0.f}, .mode = SplineNodeMode::Corner},
                    SplineNode{.position = {3.f, 0.f, 0.f}, .mode = SplineNodeMode::Corner}};

    CHECK(Editor::ExtendSpline(spline, /*atStart=*/ false, glm::vec3(3.f, 0.f, 4.f)) == 2);
    CHECK(Editor::ExtendSpline(spline, /*atStart=*/ true, glm::vec3(-2.f, 0.f, 0.f)) == 0);
    REQUIRE(spline.nodes.size() == 4);
    CheckNear(spline.nodes[0].position, glm::vec3(-2.f, 0.f, 0.f));
    CheckNear(spline.nodes[3].position, glm::vec3(3.f, 0.f, 4.f));
    CHECK(spline.nodes[0].mode == SplineNodeMode::Corner);
    CHECK(spline.nodes[3].mode == SplineNodeMode::Corner);

    // A Bezier end grows a Bezier node that starts with the handles a Smooth one
    // would have there, rather than none, which would make a sharp corner.
    Spline curve = Arch(SplineNodeMode::Bezier);
    const std::size_t added = Editor::ExtendSpline(curve, /*atStart=*/ false, glm::vec3(4.f, 2.f, -10.f));
    CHECK(curve.nodes[added].mode == SplineNodeMode::Bezier);
    CHECK(glm::length(curve.nodes[added].handleIn) > 0.f);
}

TEST_CASE("SplineEdit: an empty spline extends with its first node")
{
    Spline empty;
    CHECK(Editor::ExtendSpline(empty, /*atStart=*/ false, glm::vec3(1.f, 2.f, 3.f)) == 0);
    REQUIRE(empty.nodes.size() == 1);
    CheckNear(empty.nodes[0].position, glm::vec3(1.f, 2.f, 3.f));
    CHECK(empty.nodes[0].mode == SplineNodeMode::Smooth);
}

TEST_CASE("SplineEdit: dragging a handle keeps its partner in line, and dragging a node carries both")
{
    Spline spline = Arch(SplineNodeMode::Bezier);
    spline.nodes[1].handleIn = {-1.f, 0.f, 0.f};
    spline.nodes[1].handleOut = {2.f, 0.f, 0.f};

    // The out-handle's tip moved to straight above the node.
    Editor::MovePart(spline, SplinePart{.node = 1, .kind = SplinePartKind::HandleOut}, glm::vec3(0.f, 4.f, -10.f));
    CheckNear(spline.nodes[1].handleOut, glm::vec3(0.f, 3.f, 0.f));
    CheckNear(spline.nodes[1].handleIn, glm::vec3(0.f, -1.f, 0.f)); // opposite, at its own length

    Editor::MovePart(spline, SplinePart{.node = 1, .kind = SplinePartKind::HandleIn}, glm::vec3(-2.f, 1.f, -10.f));
    CheckNear(spline.nodes[1].handleIn, glm::vec3(-2.f, 0.f, 0.f));
    CheckNear(spline.nodes[1].handleOut, glm::vec3(3.f, 0.f, 0.f));

    Editor::MovePart(spline, SplinePart{.node = 1, .kind = SplinePartKind::Node}, glm::vec3(0.f, 5.f, -10.f));
    CheckNear(spline.nodes[1].position, glm::vec3(0.f, 5.f, -10.f));
    CheckNear(Editor::PartPosition(spline, SplinePart{.node = 1, .kind = SplinePartKind::HandleOut}),
              glm::vec3(3.f, 5.f, -10.f));
}

TEST_CASE("SplineEdit: removing a node leaves the rest")
{
    Spline spline = Arch(SplineNodeMode::Smooth);
    Editor::RemoveSplineNode(spline, 1);
    REQUIRE(spline.nodes.size() == 2);
    CheckNear(spline.nodes[1].position, glm::vec3(2.f, 0.f, -10.f));
}

TEST_CASE("SplineEdit: a ray finds the level plane in front of it, and misses one behind or beside it")
{
    const std::optional<glm::vec3> down =
        Editor::RayOnLevel(glm::vec3(0.f, 10.f, 0.f), glm::normalize(glm::vec3(0.f, -1.f, -1.f)), 0.f);
    REQUIRE(down.has_value());
    CheckNear(*down, glm::vec3(0.f, 0.f, -10.f));

    CHECK_FALSE(Editor::RayOnLevel(glm::vec3(0.f, 10.f, 0.f), glm::vec3(0.f, 1.f, 0.f), 0.f).has_value());
    CHECK_FALSE(Editor::RayOnLevel(glm::vec3(0.f, 10.f, 0.f), glm::vec3(1.f, 0.f, 0.f), 0.f).has_value());
}

TEST_CASE("SplineEdit: an insert made in a gesture undoes and redoes in one step")
{
    ECS::Scene scene;
    const ECS::Entity entity = scene.Create();
    Runtime::Path path;
    path.spline = Arch(SplineNodeMode::Smooth);
    REQUIRE(scene.Add(entity, path) != nullptr);
    Editor::EditHistory history(scene);
    const Core::Reflect::ComponentId id = Core::Reflect::ComponentIdOf<Runtime::Path>();

    history.RecordBefore(entity, id, "Add Spline Node", entity);
    (void)Editor::InsertSplineNode(scene.Get<Runtime::Path>(entity)->spline, 0.5f);
    history.CommitGesture(entity, id);
    REQUIRE(scene.Get<Runtime::Path>(entity)->spline.nodes.size() == 4);

    history.Undo();
    const Spline &undone = scene.Get<Runtime::Path>(entity)->spline;
    REQUIRE(undone.nodes.size() == 3);
    for (std::size_t i = 0; i < undone.nodes.size(); ++i)
    {
        CHECK(undone.nodes[i].position == path.spline.nodes[i].position);
        CHECK(undone.nodes[i].handleOut == path.spline.nodes[i].handleOut);
    }

    history.Redo();
    CHECK(scene.Get<Runtime::Path>(entity)->spline.nodes.size() == 4);
}
