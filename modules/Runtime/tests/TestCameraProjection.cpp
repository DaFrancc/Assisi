/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCameraProjection.cpp
/// @brief A camera projects with the scene's depth layout, which every depth
///        test, clear and read in the renderer assumes.

#include <doctest/doctest.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/SceneDepth.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>

using namespace Assisi;

namespace
{

/// What the depth buffer holds for a point @p distance in front of the camera.
float StoredDepth(const glm::mat4 &projection, float distance)
{
    const glm::vec4 clip = projection * glm::vec4(0.f, 0.f, -distance, 1.f);
    return clip.z / clip.w;
}

} // namespace

TEST_CASE("A camera stores its near plane at the scene's near depth and its far plane at the far depth")
{
    Runtime::Camera camera;
    camera.nearZ = 0.5f;
    camera.farZ = 3000.f;
    const glm::mat4 projection = Runtime::ProjectionMatrix(camera, 16.f / 9.f);

    CHECK(StoredDepth(projection, camera.nearZ) == doctest::Approx(Render::kSceneDepthNear));
    CHECK(StoredDepth(projection, camera.farZ) == doctest::Approx(Render::kSceneDepthFar));
}
