/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SplineEdit.hpp
/// @brief What shaping a spline in the viewport does to it: which node or
///        handle a click lands on, where a click on the curve falls, and the
///        nodes inserting, extending and dragging make.
///
/// Kept apart from the panels that call it, as ListEdit is, so each of these
/// can be checked without a window. Positions are in the spline's own space
/// unless a function takes a `model`, which carries that space into the
/// overlay's: measured from the camera, as everything picked is.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <Assisi/Core/Reflect/ComponentId.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/Editor/ListEdit.hpp>
#include <Assisi/Editor/ScenePick.hpp>
#include <Assisi/Geometry/Spline.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Editor
{

/// @brief How far the chain a spline is drawn and clicked as may stray from
///        the curve, in the spline's own units. One value for both, so what is
///        clickable is what is drawn; a centimetre is finer than a line on
///        screen from anywhere a spline is shaped.
inline constexpr float kSplineDrawTolerance = 0.01f;

/// @brief How near the cursor must be to a node's or handle's marker to grab
///        it, in pixels. Wider than a line's tolerance because a marker is a
///        point, which is harder to land on than a line.
inline constexpr float kSplinePartPickPixels = 8.f;

/// @brief Which piece of a node a click or a drag means.
enum class SplinePartKind : std::uint8_t
{
    Node,      ///< The node itself, carrying its handles with it.
    HandleIn,  ///< The tip of a Bezier node's in-handle.
    HandleOut, ///< The tip of a Bezier node's out-handle.
    Count_
};

/// @brief One grabbable point of a spline.
struct SplinePart
{
    std::size_t node = 0;
    SplinePartKind kind = SplinePartKind::Node;

    bool operator==(const SplinePart &) const = default;
};

/// @brief What a node or handle is drawn as. Every node is an arrowhead along
///        the way the curve travels, so the direction reads at each one; the
///        ends add a flat disc, at the base for the start and at the tip for
///        the end, the way a line is closed off.
enum class SplineMarker : std::uint8_t
{
    Start,  ///< The first node: an arrowhead with a disc at its base.
    Middle, ///< Any node between: a plain arrowhead.
    End,    ///< The last node of an open spline: an arrowhead with a disc at its tip.
    Handle, ///< The tip of a Bezier node's handle: a small box.
    Count_
};

/// @brief Half an arrowhead's length, in the spline's world units.
inline constexpr float kArrowheadHalfLength = 0.18f;

/// @brief The radius of an end's disc: wider than the arrowhead's rim, so the
///        disc shows round it from any side.
inline constexpr float kEndDiscRadius = 0.27f;

/// @brief Half the side of a handle's box.
inline constexpr float kHandleHalfExtent = 0.08f;

/// @brief Which marker @p part of @p spline is drawn as.
[[nodiscard]] SplineMarker MarkerOf(const Geometry::Spline &spline, SplinePart part);

/// @brief Half the side of the cube about its point that @p marker fits in,
///        which is what a click is tested against, so what is drawn is what is
///        clickable.
[[nodiscard]] float MarkerHalfExtent(SplineMarker marker);

/// @brief A node or handle a click landed on, and how far from it on screen.
struct SplinePartHit
{
    SplinePart part;
    float pixels = 0.f;
};

/// @brief The point of a spline the viewport is shaping: which entity, which
///        of its components, the way from that component to the spline among
///        its fields, and the node or handle.
///
/// A path rather than an address, because the component's storage and the
/// lists inside it move when anything is added; ResolveFieldPath finds the
/// spline again each frame, and refuses one an undo has taken away.
struct SplineTarget
{
    std::vector<FieldStep> path;
    SplinePart part;
    ECS::Entity entity;
    Core::Reflect::ComponentId component;

    bool operator==(const SplineTarget &) const = default;
};

/// @brief Where a click on a curve landed.
struct SplineCurveHit
{
    float parameter = 0.f; ///< Where along the spline.
    float distance = 0.f;  ///< How far from the eye, to weigh against other picks.
};

/// @brief Where @p part is, in the spline's own space.
[[nodiscard]] glm::vec3 PartPosition(const Geometry::Spline &spline, SplinePart part);

/// @brief Moves @p part to @p position, in the spline's own space.
///
/// A node carries its handles, which are relative to it. A handle keeps its
/// partner in line, pointing the opposite way at the partner's own length, so
/// the curve stays smooth through the node. The spline is settled afterwards.
void MovePart(Geometry::Spline &spline, SplinePart part, const glm::vec3 &position);

/// @brief The node or Bezier handle tip of @p spline under @p cursor: one whose
///        marker the ray passes through, or whose point is within
///        kSplinePartPickPixels on screen; the nearest on screen when several are.
///
/// The marker is the whole of what is drawn, so a node up close is clickable
/// all over it; the pixels are for one far away, whose marker is smaller than a
/// cursor can aim at. Only Bezier nodes offer their handles, since only theirs
/// are drawn and only theirs mean anything.
[[nodiscard]] std::optional<SplinePartHit> PickSplinePart(const PickRay &ray, glm::vec2 cursor,
                                                          const glm::mat4 &model, const Geometry::Spline &spline);

/// @brief Where @p cursor lands on the drawn curve of @p spline, if within
///        kOutlinePickPixels of it; the nearest to the eye when several stretches
///        are that close.
///
/// The parameter is perspective-correct: the far half of a long straight
/// stretch takes fewer pixels than the near half, so the fraction along the
/// line on screen is not the fraction along it in the world.
[[nodiscard]] std::optional<SplineCurveHit> PickSplineCurve(const PickRay &ray, glm::vec2 cursor,
                                                            const glm::mat4 &model, const Geometry::Spline &spline);

/// @brief Adds a node at @p parameter and gives its index.
///
/// The new node takes the mode of the node before it. A Bezier one splits the
/// piece exactly, the neighbours' facing handles shortened to match, so between
/// two Bezier nodes the curve keeps its shape.
std::size_t InsertSplineNode(Geometry::Spline &spline, float parameter);

/// @brief The mode after @p mode in the order a double-click steps through:
///        Corner, Smooth, Bezier, and round to Corner.
[[nodiscard]] Geometry::SplineNodeMode NextMode(Geometry::SplineNodeMode mode);

/// @brief Steps node @p index of @p spline to its next mode (NextMode). A node
///        becoming Bezier starts from the handles it drew with, so the curve
///        does not move.
void CycleNodeMode(Geometry::Spline &spline, std::size_t index);

/// @brief Adds a node at @p position past the first node when @p atStart, or
///        past the last otherwise, and gives its index.
///
/// The new node takes the mode of the end it extends, starting from the
/// handles a Smooth node would have there, so a Bezier path grows without a
/// kink.
std::size_t ExtendSpline(Geometry::Spline &spline, bool atStart, const glm::vec3 &position);

/// @brief Removes node @p index, which must exist, and settles what is left.
void RemoveSplineNode(Geometry::Spline &spline, std::size_t index);

/// @brief Where the ray from @p origin along @p direction crosses the level
///        plane at @p height, if it does, in front of the origin.
[[nodiscard]] std::optional<glm::vec3> RayOnLevel(const glm::vec3 &origin, const glm::vec3 &direction,
                                                  float height);

} // namespace Assisi::Editor
