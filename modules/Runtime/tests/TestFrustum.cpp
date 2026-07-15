/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Frustum.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>

using Assisi::Geometry::Aabb;
using Assisi::Render::Frustum;
using Assisi::Runtime::Camera;
using Assisi::Runtime::ProjectionMatrix;
using Assisi::Runtime::Transform;
using Assisi::Runtime::ViewMatrix;

namespace
{
// A frustum for a camera at the origin looking down -Z (identity transform).
Frustum CameraFrustum()
{
    Camera camera;
    camera.fovDegrees = 60.f;
    camera.nearZ      = 0.1f;
    camera.farZ       = 200.f;

    Transform camTransform;
    camTransform.worldMatrix = glm::mat4(1.f); // origin, looking down -Z

    const glm::mat4 view       = ViewMatrix(camTransform);
    const glm::mat4 projection = ProjectionMatrix(camera, 16.f / 9.f);
    return Frustum::FromViewProjection(projection * view);
}

Aabb BoxAt(const glm::vec3 &center, float halfSize)
{
    return Aabb{.min = center - glm::vec3(halfSize), .max = center + glm::vec3(halfSize)};
}
} // namespace

TEST_CASE("Frustum::IntersectsAabb accepts a box in front of the camera")
{
    const Frustum frustum = CameraFrustum();
    CHECK(frustum.IntersectsAabb(BoxAt(glm::vec3(0.f, 0.f, -5.f), 1.f)));
}

TEST_CASE("Frustum::IntersectsAabb rejects a box behind the camera")
{
    const Frustum frustum = CameraFrustum();
    // Positive Z is behind a camera that looks down -Z: fully past the near plane.
    CHECK_FALSE(frustum.IntersectsAabb(BoxAt(glm::vec3(0.f, 0.f, 5.f), 1.f)));
}

TEST_CASE("Frustum::IntersectsAabb rejects a box beyond the far plane")
{
    const Frustum frustum = CameraFrustum();
    CHECK_FALSE(frustum.IntersectsAabb(BoxAt(glm::vec3(0.f, 0.f, -500.f), 1.f))); // farZ = 200
}

TEST_CASE("Frustum::IntersectsAabb rejects a box far off to the side")
{
    const Frustum frustum = CameraFrustum();
    CHECK_FALSE(frustum.IntersectsAabb(BoxAt(glm::vec3(100.f, 0.f, -5.f), 1.f)));
}

TEST_CASE("Frustum::IntersectsAabb is tighter than the sphere at a corner")
{
    const Frustum frustum = CameraFrustum();
    // A thin box hugging the right frustum edge but just outside it: the box is
    // rejected, where its (larger) bounding sphere might not be. This is the whole
    // point of the AABB refine — fewer false "visible".
    const Aabb box = BoxAt(glm::vec3(60.f, 0.f, -5.f), 0.5f);
    CHECK_FALSE(frustum.IntersectsAabb(box));
    // Sanity: a box straddling the same edge but reaching inside is kept.
    CHECK(frustum.IntersectsAabb(Aabb{.min = glm::vec3(-1.f, -1.f, -6.f), .max = glm::vec3(1.f, 1.f, -4.f)}));
}
