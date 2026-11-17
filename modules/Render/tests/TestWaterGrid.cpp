/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Render/Frustum.hpp>
#include <Assisi/Render/WaterGrid.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace Assisi::Render;

namespace
{

/// The nodes holding @p point, measured from the camera in the region's frame.
std::vector<const WaterNode *> NodesAt(const std::vector<WaterNode> &nodes, glm::vec2 point)
{
    std::vector<const WaterNode *> holding;
    for (const WaterNode &node : nodes)
    {
        if (point.x >= node.corner.x && point.x < node.corner.x + node.size && point.y >= node.corner.y &&
            point.y < node.corner.y + node.size)
        {
            holding.push_back(&node);
        }
    }
    return holding;
}

float DistanceFromCamera(glm::vec2 point, double height)
{
    return std::sqrt(point.x * point.x + point.y * point.y + static_cast<float>(height * height));
}

/// A point just across each edge of @p node, at a few places along it.
std::vector<glm::vec2> PointsAcrossEdges(const WaterNode &node, float step)
{
    std::vector<glm::vec2> points;
    for (const float along : {0.125f, 0.375f, 0.625f, 0.875f})
    {
        const float offset = node.size * along;
        points.push_back(node.corner + glm::vec2(offset, -step));
        points.push_back(node.corner + glm::vec2(offset, node.size + step));
        points.push_back(node.corner + glm::vec2(-step, offset));
        points.push_back(node.corner + glm::vec2(node.size + step, offset));
    }
    return points;
}

/// The ocean at sea level, seen from a few metres up.
WaterRegion Sea()
{
    return WaterRegion{.origin = glm::dvec3(0.0)};
}

WaterGridView ViewFrom(glm::dvec3 camera, float drawDistance)
{
    return WaterGridView{.camera = camera, .drawDistance = drawDistance};
}

} // namespace

TEST_CASE("The patches tile the sea around the camera without gaps or overlaps")
{
    std::vector<WaterNode> nodes;
    const float reach = 3000.0f;
    SelectWaterNodes(Sea(), ViewFrom(glm::dvec3(12.3, 4.0, -7.9), reach), nodes);
    REQUIRE(!nodes.empty());
    // About thirty a level all the way round the camera; the frustum keeps a
    // third of them.
    CHECK(nodes.size() < 450u);

    // Inside the draw distance every point is in exactly one patch.
    const float inner = reach * 0.6f;
    constexpr int32_t kSamples = 81;
    for (int32_t z = 0; z < kSamples; ++z)
    {
        for (int32_t x = 0; x < kSamples; ++x)
        {
            const glm::vec2 point = glm::vec2(static_cast<float>(x), static_cast<float>(z)) *
                                    (2.0f * inner / static_cast<float>(kSamples - 1)) -
                                    glm::vec2(inner) + glm::vec2(0.37f, 0.61f);
            CAPTURE(point.x);
            CAPTURE(point.y);
            CHECK(NodesAt(nodes, point).size() == 1u);
        }
    }
}

TEST_CASE("Neighbouring patches differ by at most one level, and the blend between them leaves no crack")
{
    for (const double height : {2.0, 60.0})
    {
        CAPTURE(height);
        std::vector<WaterNode> nodes;
        SelectWaterNodes(Sea(), ViewFrom(glm::dvec3(-40.5, height, 210.25), 5000.0f), nodes);
        uint32_t borders = 0u;
        for (const WaterNode &node : nodes)
        {
            for (const glm::vec2 outside : PointsAcrossEdges(node, kWaterFinestSpacing * 0.5f))
            {
                const std::vector<const WaterNode *> across = NodesAt(nodes, outside);
                if (across.empty())
                {
                    continue;
                }
                const WaterNode &neighbour = *across.front();
                const int32_t step = static_cast<int32_t>(neighbour.level) - static_cast<int32_t>(node.level);
                CHECK(std::abs(step) <= 1);
                if (step == 1)
                {
                    // On the shared edge the finer patch has blended all the way
                    // to the coarser level, and the coarser not begun to blend.
                    const float distance = DistanceFromCamera(outside, height);
                    CHECK(WaterMorph(distance, node.level) == doctest::Approx(1.0f));
                    CHECK(WaterMorph(distance, neighbour.level) == doctest::Approx(0.0f));
                    ++borders;
                }
            }
        }
        CHECK(borders > 0u);
    }
}

TEST_CASE("The finest patches surround the camera, and rise with it")
{
    std::vector<WaterNode> nodes;
    SelectWaterNodes(Sea(), ViewFrom(glm::dvec3(5.0, 1.5, 5.0), 3000.0f), nodes);
    const std::vector<const WaterNode *> below = NodesAt(nodes, glm::vec2(0.0f));
    REQUIRE(below.size() == 1u);
    CHECK(below.front()->level == 0u);

    SelectWaterNodes(Sea(), ViewFrom(glm::dvec3(5.0, 500.0, 5.0), 3000.0f), nodes);
    const std::vector<const WaterNode *> belowHigh = NodesAt(nodes, glm::vec2(0.0f));
    REQUIRE(belowHigh.size() == 1u);
    CHECK(belowHigh.front()->level > 2u);
}

TEST_CASE("Nothing is drawn past the draw distance or with the camera that far above")
{
    std::vector<WaterNode> nodes;
    const glm::dvec3 camera(100.0, 3.0, -50.0);
    SelectWaterNodes(Sea(), ViewFrom(camera, 1000.0f), nodes);
    for (const WaterNode &node : nodes)
    {
        const glm::vec2 nearest = glm::clamp(glm::vec2(0.0f), node.corner, node.corner + glm::vec2(node.size));
        CHECK(DistanceFromCamera(nearest, camera.y) < 1000.0f);
    }

    SelectWaterNodes(Sea(), ViewFrom(glm::dvec3(0.0, 2000.0, 0.0), 1000.0f), nodes);
    CHECK(nodes.empty());
}

TEST_CASE("A lake's patches stay over the lake and cover all of it, however it is turned")
{
    const float turn = glm::radians(30.0f);
    const WaterRegion lake{.origin = glm::dvec3(500.0, 140.0, -300.0),
                           .axisX = glm::vec2(std::cos(turn), -std::sin(turn)),
                           .halfExtent = glm::vec2(40.0f, 25.0f)};
    const glm::dvec3 camera(520.0, 145.0, -310.0);
    std::vector<WaterNode> nodes;
    SelectWaterNodes(lake, ViewFrom(camera, 2000.0f), nodes);
    REQUIRE(!nodes.empty());

    const glm::vec2 cameraInLake(RegionPosition(lake, camera));
    const glm::vec2 lakeMin = -lake.halfExtent - cameraInLake;
    const glm::vec2 lakeMax = lake.halfExtent - cameraInLake;
    for (const WaterNode &node : nodes)
    {
        const bool overlaps = node.corner.x < lakeMax.x && node.corner.y < lakeMax.y &&
                                                                           node.corner.x + node.size > lakeMin.x && node.corner.y + node.size > lakeMin.y;
        CHECK(overlaps);
    }
    constexpr int32_t kSamples = 21;
    for (int32_t z = 0; z < kSamples; ++z)
    {
        for (int32_t x = 0; x < kSamples; ++x)
        {
            const glm::vec2 share = glm::vec2(static_cast<float>(x), static_cast<float>(z)) / static_cast<float>(kSamples);
            // Off the patches' edges, which a share of a whole metre would land on.
            const glm::vec2 point = lakeMin + (lakeMax - lakeMin) * (share + glm::vec2(0.0137f));
            CHECK(NodesAt(nodes, point).size() == 1u);
        }
    }

    // The region's frame turns with it: its x axis is where the lake's own x is.
    const glm::dvec2 along = RegionPosition(lake, lake.origin + glm::dvec3(std::cos(turn), 0.0, -std::sin(turn)));
    CHECK(along.x == doctest::Approx(1.0));
    CHECK(along.y == doctest::Approx(0.0));
}

TEST_CASE("Patches sit on a grid fixed to the water, the same far from the origin as near it")
{
    const glm::dvec3 near(0.3, 2.0, 0.7);
    std::vector<WaterNode> nodesNear;
    SelectWaterNodes(Sea(), ViewFrom(near, 3000.0f), nodesNear);

    // Every corner is a whole number of its own size from the water's origin.
    for (const WaterNode &node : nodesNear)
    {
        const double cornerX = static_cast<double>(node.corner.x) + near.x;
        const double cornerZ = static_cast<double>(node.corner.y) + near.z;
        const double size = static_cast<double>(node.size);
        CHECK(std::abs(cornerX / size - std::round(cornerX / size)) < 1e-4);
        CHECK(std::abs(cornerZ / size - std::round(cornerZ / size)) < 1e-4);
    }

    // Moved a whole number of the coarsest node's size, the patches are the
    // same around the camera, hundreds of kilometres out.
    const double coarsest = static_cast<double>(WaterNodeSize(kMaxWaterLevels - 1u));
    const glm::dvec3 far = near + glm::dvec3(coarsest * 3.0, 0.0, -coarsest * 2.0);
    std::vector<WaterNode> nodesFar;
    SelectWaterNodes(Sea(), ViewFrom(far, 3000.0f), nodesFar);
    REQUIRE(nodesFar.size() == nodesNear.size());
    for (size_t i = 0; i < nodesNear.size(); ++i)
    {
        CHECK(nodesFar[i].level == nodesNear[i].level);
        CHECK(nodesFar[i].corner.x == doctest::Approx(nodesNear[i].corner.x).epsilon(1e-6));
        CHECK(nodesFar[i].corner.y == doctest::Approx(nodesNear[i].corner.y).epsilon(1e-6));
    }
}

TEST_CASE("Patches out of the frustum are not drawn, but those whose waves reach into it are")
{
    // Looking along +x from two metres up.
    const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 2000.0f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const Frustum frustum = Frustum::FromViewProjection(projection * view);
    WaterGridView viewFrom = ViewFrom(glm::dvec3(0.0, 2.0, 0.0), 2000.0f);

    std::vector<WaterNode> all;
    SelectWaterNodes(Sea(), viewFrom, all);
    viewFrom.frustum = &frustum;
    std::vector<WaterNode> visible;
    SelectWaterNodes(Sea(), viewFrom, visible);
    CHECK(visible.size() < all.size());
    for (const WaterNode &node : visible)
    {
        // Nothing wholly behind the camera.
        CHECK(node.corner.x + node.size > 0.0f);
    }

    // Raised waves keep a node the plane alone would lose: from high up looking
    // level, the sea's plane is under the frustum's bottom edge nearby.
    WaterGridView high = ViewFrom(glm::dvec3(0.0, 40.0, 0.0), 2000.0f);
    const glm::mat4 levelView =
        glm::lookAt(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const Frustum narrow = Frustum::FromViewProjection(glm::perspective(glm::radians(10.0f), 1.0f, 0.1f, 2000.0f) * levelView);
    high.frustum = &narrow;
    std::vector<WaterNode> flat;
    SelectWaterNodes(Sea(), high, flat);
    high.waveReach = 30.0f;
    std::vector<WaterNode> waved;
    SelectWaterNodes(Sea(), high, waved);
    CHECK(waved.size() > flat.size());
}
