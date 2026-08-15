/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/ScenePick.hpp>

#include <Assisi/Runtime/Components.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Assisi::Editor
{

namespace
{

/// Half-thickness handed to an axis a mesh is flat on. Local units, so it grows
/// with the entity's scale — the same way the rest of the box does.
constexpr float kFlatAxisHalfExtent = 0.005f;

} // namespace

bool RayAabbIntersect(glm::vec3 origin, glm::vec3 dir, const Geometry::Aabb &localBounds, const glm::mat4 &model,
                      float &tOut)
{
    const glm::mat4 inv   = glm::inverse(model);
    const glm::vec3 lOrig = glm::vec3(inv * glm::vec4(origin, 1.f));
    const glm::vec3 lDir  = glm::vec3(inv * glm::vec4(dir, 0.f));

    float tMin = -std::numeric_limits<float>::max();
    float tMax =  std::numeric_limits<float>::max();

    for (int32_t i = 0; i < 3; ++i)
    {
        if (std::abs(lDir[i]) < 1e-8f)
        {
            if (lOrig[i] < localBounds.min[i] || lOrig[i] > localBounds.max[i])
                return false;
        }
        else
        {
            float t1 = (localBounds.min[i] - lOrig[i]) / lDir[i];
            float t2 = (localBounds.max[i] - lOrig[i]) / lDir[i];
            if (t1 > t2)
                std::swap(t1, t2);
            tMin = std::max(tMin, t1);
            tMax = std::min(tMax, t2);
            if (tMin > tMax)
                return false;
        }
    }

    if (tMax < 0.f)
        return false;

    tOut = tMin > 0.f ? tMin : tMax;
    return true;
}

bool RayBillboardIntersect(glm::vec3 origin, glm::vec3 dir, glm::vec3 center, glm::vec3 right, glm::vec3 up,
                           float half, float &tOut)
{
    const glm::vec3 normal = glm::cross(right, up); // the quad faces the camera
    const float denom  = glm::dot(dir, normal);
    if (std::abs(denom) < 1e-8f)
        return false; // ray parallel to the quad

    const float t = glm::dot(center - origin, normal) / denom;
    if (t < 0.f)
        return false;

    const glm::vec3 hit    = origin + t * dir;
    const glm::vec3 offset = hit - center;
    if (std::abs(glm::dot(offset, right)) <= half && std::abs(glm::dot(offset, up)) <= half)
    {
        tOut = t;
        return true;
    }
    return false;
}

Geometry::Aabb PickableBounds(const Geometry::Aabb &bounds)
{
    const glm::vec3 extent = bounds.max - bounds.min;
    if (glm::all(glm::lessThanEqual(extent, glm::vec3(0.f))))
    {
        return kFallbackPickBounds;
    }
    // Only axes thinner than the minimum are pushed out, and symmetrically, so the
    // box keeps its centre.
    const glm::vec3 pad = glm::max(glm::vec3(kFlatAxisHalfExtent) - (extent * 0.5f), glm::vec3(0.f));
    return Geometry::Aabb{.min = bounds.min - pad, .max = bounds.max + pad};
}

std::optional<Geometry::Aabb> MeshPickBounds(const ECS::Scene &scene, ECS::Entity entity)
{
    const Runtime::MeshRenderer *mrc = scene.Get<Runtime::MeshRenderer>(entity);
    if (mrc == nullptr)
    {
        return std::nullopt;
    }
    if (mrc->meshBuffer == nullptr)
    {
        return kFallbackPickBounds;
    }
    return PickableBounds(mrc->meshBuffer->LocalAabb());
}

ECS::Entity PickEntityInScene(ECS::Scene &scene, const PickRay &ray, float iconHalf, PickBoundsLookup boundsOf,
                              float &tOut)
{
    tOut = std::numeric_limits<float>::max();
    if (!ray.valid || boundsOf == nullptr)
    {
        return ECS::NullEntity;
    }

    float closestT     = std::numeric_limits<float>::max();
    ECS::Entity result = ECS::NullEntity;

    for (auto [e, tc] : scene.Query<Runtime::Transform>())
    {
        // A meshed entity is picked by the bounds it reports; a placement-only one
        // by its icon quad alone, so it does not swallow clicks over a whole cube.
        const std::optional<Geometry::Aabb> bounds = boundsOf(scene, e);
        float t = 0.f;
        const bool hit = bounds.has_value()
                             ? RayAabbIntersect(ray.origin, ray.direction, *bounds, tc.worldMatrix, t)
                             : RayBillboardIntersect(ray.origin, ray.direction, glm::vec3(tc.worldMatrix[3]),
                                                     ray.cameraRight, ray.cameraUp, iconHalf, t);
        if (hit && t < closestT)
        {
            closestT = t;
            result   = e;
        }
    }

    tOut = closestT;
    return result;
}

} // namespace Assisi::Editor
