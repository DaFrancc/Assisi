/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CameraPath.hpp
/// @brief Scripted camera paths, and the route they form when played in order.
///
/// A benchmark needs the same frames rendered every run: the far view, the close
/// view and the fly-through, at the same moments. A camera flown by hand never
/// does that, so these components record the flight as data in the level.
///
/// Every path is a closed form of its own progress, so the pose at a given time
/// has one answer: two runs that sample the route at different moments still
/// travel the same route.
///
/// The fields are world space, not relative to the entity's Transform. A path
/// describes where the camera goes, not something that sits in the level, so
/// moving the entity that holds it moves nothing.

#include <cstdint>
#include <vector>

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Prelude.hpp>
#include <Assisi/Runtime/Components.hpp>

namespace Assisi::Runtime
{

/// @brief The camera travels in a straight line from `from` to `to`.
ACOMP()
struct CameraLinePath
{
    AFIELD() glm::vec3 from{0.f, 2.f, 10.f}; ///< Where the camera starts.
    AFIELD() glm::vec3 to{0.f, 2.f, -10.f};  ///< Where the camera ends.

    /// What the camera faces, unless it looks along the path.
    AFIELD(radioListen = {source = lookAlongPath, value = false, behavior = vanish})
    glm::vec3 lookAt{0.f, 0.f, 0.f};

    /// Seconds this path takes when the route runs at its authored length. A run
    /// of a fixed total length scales every path by the same factor.
    AFIELD(min = 0) float seconds = 5.f;

    /// Position in the route. Lower plays first; a tie goes to the lower entity
    /// index, which a given level file always loads the same way.
    AFIELD() int32_t order = 0;

    /// Faces the direction of travel when true, and `lookAt` when false. A
    /// fly-through wants the first, a dolly past something the second.
    AFIELD(radioBroadcast) bool lookAlongPath = true;
};

/// @brief The camera travels round a horizontal circle about `center`.
ACOMP()
struct CameraCirclePath
{
    AFIELD() glm::vec3 center{0.f, 0.f, 0.f};
    AFIELD() glm::vec3 lookAt{0.f, 0.f, 0.f};
    AFIELD(min = 0) float radius = 10.f;

    /// Height of the circle above `center`, so an orbit can look down on the
    /// point it circles.
    AFIELD() float height = 5.f;

    /// Where on the circle the camera starts. 0 degrees is +X; 90 is +Z.
    AFIELD() float startDegrees = 0.f;

    /// How far round it goes. 360 is one orbit; a negative sweep goes the other
    /// way.
    AFIELD() float sweepDegrees = 360.f;

    /// See CameraLinePath::seconds.
    AFIELD(min = 0) float seconds = 5.f;

    /// See CameraLinePath::order.
    AFIELD() int32_t order = 0;
};

/// @brief Where the camera is and what it looks at.
struct CameraAim
{
    glm::vec3 eye{0.f};
    glm::vec3 target{0.f, 0.f, -1.f};
};

/// @brief One path in a route: whichever component it came from, and which
/// entity holds it.
struct CameraRouteLeg
{
    CameraLinePath line;
    CameraCirclePath circle;
    ECS::Entity entity;
    bool isCircle = false;
};

/// @brief Every path in @p scene, in play order.
///
/// Paths with no length are left out: they would be passed through in no time,
/// and one the author parked at zero is one they meant to skip.
[[nodiscard]] std::vector<CameraRouteLeg> BuildCameraRoute(ECS::Scene &scene);

/// @brief The route's authored length: the sum of its paths' seconds.
[[nodiscard]] float CameraRouteSeconds(const std::vector<CameraRouteLeg> &route);

/// @brief The pose @p leg gives at @p progress, where 0 is its start and 1 its
/// end. Progress outside [0, 1] is clamped.
[[nodiscard]] CameraAim EvaluateCameraLeg(const CameraRouteLeg &leg, float progress);

/// @brief The pose the route gives @p seconds after it starts, at its authored
/// length. Before the start is the first pose; after the end is the last.
///
/// An empty route gives a default CameraAim.
[[nodiscard]] CameraAim EvaluateCameraRoute(const std::vector<CameraRouteLeg> &route, float seconds);

/// @brief A camera Transform at @p aim, with its world matrix filled in.
///
/// The world matrix is filled because Runtime::ViewMatrix reads that and
/// nothing else. Up is world +Y, except when looking straight up or down, where
/// +Y is parallel to the view and gives no roll, so +Z stands in for it.
[[nodiscard]] Transform CameraTransformFor(const CameraAim &aim);

} // namespace Assisi::Runtime
