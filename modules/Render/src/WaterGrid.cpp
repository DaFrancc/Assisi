/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/WaterGrid.hpp>

#include <Assisi/Geometry/Bounds.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>

namespace Assisi::Render
{
namespace
{

/// What one walk measures every node against.
struct Walk
{
    /// The covered rectangle, in the region's frame.
    glm::dvec2 coverMin{0.0};
    glm::dvec2 coverMax{0.0};
    /// The camera in the region's frame, and its height above the water.
    glm::dvec2 camera{0.0};
    double height = 0.0;
    const WaterRegion *region = nullptr;
    const WaterGridView *view = nullptr;
    std::vector<WaterNode> *nodes = nullptr;
};

/// The distance from the camera to the nearest point of a node.
double DistanceToNode(const Walk &walk, glm::dvec2 corner, double size)
{
    const double dx = std::max({corner.x - walk.camera.x, 0.0, walk.camera.x - (corner.x + size)});
    const double dz = std::max({corner.y - walk.camera.y, 0.0, walk.camera.y - (corner.y + size)});
    return std::sqrt(dx * dx + dz * dz + walk.height * walk.height);
}

/// Whether any of a node, raised and lowered by the waves, is in the frustum.
bool InFrustum(const Walk &walk, glm::dvec2 corner, double size)
{
    if (walk.view->frustum == nullptr)
    {
        return true;
    }
    const glm::vec2 axisX = walk.region->axisX;
    const glm::vec2 axisZ(-axisX.y, axisX.x);
    const glm::vec2 low(corner - walk.camera);
    const glm::vec2 high(corner + glm::dvec2(size) - walk.camera);
    glm::vec2 boxMin(std::numeric_limits<float>::max());
    glm::vec2 boxMax(-std::numeric_limits<float>::max());
    for (const glm::vec2 local : {low, glm::vec2(high.x, low.y), glm::vec2(low.x, high.y), high})
    {
        const glm::vec2 world = local.x * axisX + local.y * axisZ;
        boxMin = glm::min(boxMin, world);
        boxMax = glm::max(boxMax, world);
    }
    const float surface = static_cast<float>(-walk.height);
    const float reach = walk.view->waveReach;
    return walk.view->frustum->IntersectsAabb(Geometry::Aabb{.min = glm::vec3(boxMin.x - reach, surface - reach, boxMin.y - reach),
                                                             .max = glm::vec3(boxMax.x + reach, surface + reach, boxMax.y + reach)});
}

void Visit(const Walk &walk, glm::dvec2 corner, uint32_t level)
{
    const double size = static_cast<double>(WaterNodeSize(level));
    const bool covers = corner.x < walk.coverMax.x && corner.y < walk.coverMax.y && corner.x + size > walk.coverMin.x &&
                                   corner.y + size > walk.coverMin.y;
    const double distance = DistanceToNode(walk, corner, size);
    if (!covers || distance >= static_cast<double>(walk.view->drawDistance) || !InFrustum(walk, corner, size))
    {
        return;
    }
    if (level > 0u && distance < static_cast<double>(WaterLodRange(level - 1u)))
    {
        const double half = size * 0.5;
        for (const glm::dvec2 child : {corner, corner + glm::dvec2(half, 0.0), corner + glm::dvec2(0.0, half),
                                       corner + glm::dvec2(half)})
        {
            Visit(walk, child, level - 1u);
        }
        return;
    }
    walk.nodes->push_back(WaterNode{.corner = glm::vec2(corner - walk.camera), .size = static_cast<float>(size),
                                    .level = level});
}

} // namespace

float WaterNodeSize(uint32_t level)
{
    return static_cast<float>(kWaterPatchQuads) * kWaterFinestSpacing * std::exp2(static_cast<float>(level));
}

float WaterLodRange(uint32_t level)
{
    return kWaterRangeScale * WaterNodeSize(level);
}

float WaterMorph(float distance, uint32_t level)
{
    const float range = WaterLodRange(level);
    const float start = range * kWaterMorphStart;
    return std::clamp((distance - start) / (range - start), 0.0f, 1.0f);
}

glm::dvec2 RegionPosition(const WaterRegion &region, glm::dvec3 world)
{
    const glm::dvec2 offset(world.x - region.origin.x, world.z - region.origin.z);
    const glm::dvec2 axisX(region.axisX);
    const glm::dvec2 axisZ(-axisX.y, axisX.x);
    return glm::dvec2(glm::dot(offset, axisX), glm::dot(offset, axisZ));
}

void SelectWaterNodes(const WaterRegion &region, const WaterGridView &view, std::vector<WaterNode> &nodes)
{
    nodes.clear();
    const double reach = static_cast<double>(view.drawDistance);
    Walk walk{.camera = RegionPosition(region, view.camera),
              .height = view.camera.y - region.origin.y,
              .region = &region,
              .view = &view,
              .nodes = &nodes};
    if (reach <= 0.0 || std::abs(walk.height) >= reach)
    {
        return;
    }
    walk.coverMin = walk.camera - glm::dvec2(reach);
    walk.coverMax = walk.camera + glm::dvec2(reach);
    const bool endless = region.halfExtent.x <= 0.0f || region.halfExtent.y <= 0.0f;
    if (!endless)
    {
        walk.coverMin = glm::max(walk.coverMin, -glm::dvec2(region.halfExtent));
        walk.coverMax = glm::min(walk.coverMax, glm::dvec2(region.halfExtent));
        if (walk.coverMin.x >= walk.coverMax.x || walk.coverMin.y >= walk.coverMax.y)
        {
            return;
        }
    }

    // The coarsest level is the first whose nodes span half the covered width,
    // so a handful of roots tile it.
    const glm::dvec2 cover = walk.coverMax - walk.coverMin;
    const double needed = 0.5 * std::max(cover.x, cover.y);
    uint32_t top = 0u;
    while (top + 1u < kMaxWaterLevels && static_cast<double>(WaterNodeSize(top)) < needed)
    {
        ++top;
    }
    const double rootSize = static_cast<double>(WaterNodeSize(top));
    const glm::dvec2 first = glm::floor(walk.coverMin / rootSize);
    const glm::dvec2 last = glm::ceil(walk.coverMax / rootSize);
    for (double z = first.y; z < last.y; z += 1.0)
    {
        for (double x = first.x; x < last.x; x += 1.0)
        {
            Visit(walk, glm::dvec2(x, z) * rootSize, top);
        }
    }
}

} // namespace Assisi::Render
