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

/// Clip-space w at or below this is the eye plane and behind it, where the
/// perspective divide stops meaning anything.
constexpr float kMinClipW = 1e-4f;

/// Where a clip-space point lands in framebuffer pixels. Y is flipped because a
/// mouse position counts down from the top and NDC counts up from the bottom.
glm::vec2 ClipToScreen(const glm::vec4 &clip, glm::vec2 viewport)
{
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    return glm::vec2((ndc.x * 0.5f + 0.5f) * viewport.x, (0.5f - ndc.y * 0.5f) * viewport.y);
}

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

bool ScreenDistanceToSegment(const PickRay &ray, glm::vec2 cursor, glm::vec3 a, glm::vec3 b, float &pixelsOut,
                             float &distanceOut)
{
    if (!ray.valid || ray.viewportSize.x <= 0.f || ray.viewportSize.y <= 0.f)
    {
        return false;
    }

    const glm::vec4 clipA = ray.viewProjection * glm::vec4(a, 1.f);
    const glm::vec4 clipB = ray.viewProjection * glm::vec4(b, 1.f);
    if (clipA.w < kMinClipW && clipB.w < kMinClipW)
    {
        return false;
    }

    // The stretch of the segment that is in front of the eye, as a range of its
    // own parameter. Clipping here rather than after the divide is the whole
    // point: w changes sign across the eye plane, and the endpoint past it comes
    // back projected through the origin onto the opposite side of the screen.
    float visibleFrom = 0.f;
    float visibleTo   = 1.f;
    if (clipA.w < kMinClipW)
    {
        visibleFrom = (kMinClipW - clipA.w) / (clipB.w - clipA.w);
    }
    else if (clipB.w < kMinClipW)
    {
        visibleTo = (kMinClipW - clipA.w) / (clipB.w - clipA.w);
    }

    // A straight line in the world projects to a straight line on screen, so the
    // clipped ends are all this needs — the distance below is exact, not sampled.
    const glm::vec4 clip0 = glm::mix(clipA, clipB, visibleFrom);
    const glm::vec4 clip1 = glm::mix(clipA, clipB, visibleTo);
    const glm::vec2 end0  = ClipToScreen(clip0, ray.viewportSize);
    const glm::vec2 end1  = ClipToScreen(clip1, ray.viewportSize);

    const glm::vec2 span    = end1 - end0;
    const float spanLengthSq = glm::dot(span, span);
    const float onScreen =
        spanLengthSq > 0.f ? std::clamp(glm::dot(cursor - end0, span) / spanLengthSq, 0.f, 1.f) : 0.f;
    pixelsOut = glm::length(cursor - (end0 + onScreen * span));

    // How far along the screen is not how far along the segment: perspective packs
    // the far half of a line into fewer pixels, and the two w's are what undo it.
    // Without this the reported depth is of some other point on the line, and the
    // outline wins or loses against a wall it is not actually beside.
    const float weighted = clip1.w + onScreen * (clip0.w - clip1.w);
    const float alongClipped = weighted != 0.f ? (onScreen * clip0.w) / weighted : onScreen;

    const glm::vec3 nearest = glm::mix(a, b, glm::mix(visibleFrom, visibleTo, alongClipped));
    distanceOut             = glm::distance(ray.origin, nearest);
    return true;
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
