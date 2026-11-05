/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestSceneDepth.cpp
/// @brief The scene's projection stores the near plane at one and the far
///        plane at zero, and in float depth that keeps surfaces close
///        together in order from a hand's width away to the horizon.

#include <doctest/doctest.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/SceneDepth.hpp>

using namespace Assisi::Render;

namespace
{

/// A 60-degree lens on a 16:9 target, near the editor camera's near plane and
/// far enough to see 20 km.
constexpr float kFovRadians = 1.0471976f;
constexpr float kAspect = 16.f / 9.f;
constexpr float kNear = 0.1f;
constexpr float kFar = 20000.f;

/// How far behind a surface a second one sits, as a fraction of its distance:
/// 1 mm at 10 m, 2 m at 20 km.
constexpr float kSeparation = 1e-4f;

/// Each distance checked is this many times the one before.
constexpr float kDistanceStep = 1.25f;

/// What a 32-bit float depth buffer holds for a point @p distance in front of
/// the camera, computed in single precision as the GPU does.
float StoredDepth(const glm::mat4 &projection, float distance)
{
    const glm::vec4 clip = projection * glm::vec4(0.f, 0.f, -distance, 1.f);
    return clip.z / clip.w;
}

/// Whether depth @p a is nearer the camera than depth @p b.
bool Nearer(float a, float b)
{
    return kSceneDepthNear > kSceneDepthFar ? a > b : a < b;
}

} // namespace

TEST_CASE("The scene's projection stores the near plane at one and the far plane at zero")
{
    const glm::mat4 projection = ScenePerspective(kFovRadians, kAspect, kNear, kFar);
    CHECK(StoredDepth(projection, kNear) == doctest::Approx(kSceneDepthNear));
    CHECK(StoredDepth(projection, kFar) == doctest::Approx(kSceneDepthFar));
    CHECK(Nearer(StoredDepth(projection, 1.f), StoredDepth(projection, 2.f)));
}

TEST_CASE("Two surfaces a ten-thousandth of their distance apart keep their order from 0.1 m to 20 km")
{
    // With the far plane at one, a surface 10 km out and one 1 m behind it
    // round to the same float, and the two flicker through each other.
    const glm::mat4 projection = ScenePerspective(kFovRadians, kAspect, kNear, kFar);
    for (float distance = kNear; distance * (1.f + kSeparation) < kFar; distance *= kDistanceStep)
    {
        CAPTURE(distance);
        const float front = StoredDepth(projection, distance);
        const float behind = StoredDepth(projection, distance * (1.f + kSeparation));
        CHECK(Nearer(front, behind));
    }
}
