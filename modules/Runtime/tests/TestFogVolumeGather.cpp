/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestFogVolumeGather.cpp
/// @brief The scene's fog volume components become the frame's volumes: each
/// shape as its kind, placed, turned and scaled with its entity, and left out
/// when off screen or without thickness.

#include <doctest/doctest.h>

#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Geometry/Spline.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/FogVolume.hpp>
#include <Assisi/Runtime/FogVolume.hpp>
#include <Assisi/Runtime/FogVolumeGather.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

using namespace Assisi;
using Assisi::ECS::Transform;
using Assisi::Runtime::FogBox;
using Assisi::Runtime::FogOutline;
using Assisi::Runtime::FogSphere;

namespace
{

constexpr uint32_t kWidth = 1920u;
constexpr uint32_t kHeight = 1080u;
constexpr float kForever = std::numeric_limits<float>::infinity();

/// The eye at the origin, looking down -z.
Runtime::FogVolumeView View()
{
    const glm::mat4 projection = glm::perspective(glm::radians(60.f), 16.f / 9.f, 0.1f, 1000.f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.f), glm::vec3(0.f, 0.f, -1.f), glm::vec3(0.f, 1.f, 0.f));
    return Runtime::FogVolumeView{.viewProjection = projection * view, .width = kWidth, .height = kHeight};
}

ECS::Entity Place(ECS::Scene &scene, const glm::dvec3 &position, const glm::vec3 &scale = glm::vec3(1.f))
{
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, Transform{.position = position, .scale = scale}) != nullptr);
    return entity;
}

/// An outline of Corner nodes, so it is exactly the polygon through them.
Geometry::Spline Polygon(std::span<const glm::vec2> points)
{
    Geometry::Spline spline;
    spline.closed = true;
    for (const glm::vec2 &point : points)
    {
        spline.nodes.push_back(
            Geometry::SplineNode{.position = glm::vec3(point.x, 0.f, point.y), .mode = Geometry::SplineNodeMode::Corner});
    }
    return spline;
}

Render::FogVolumeFrame Gather(ECS::Scene &scene)
{
    (void)ECS::PropagateTransforms(scene, 0u);
    Render::FogVolumeFrame frame;
    (void)Runtime::GatherFogVolumes(scene, View(), frame);
    return frame;
}

/// Metres of fog straight down through @p x, @p z, from well above.
float Down(const Render::FogVolumeFrame &frame, std::size_t volume, float x, float z)
{
    return Render::FogVolumeSpan(frame.volumes[volume], frame.edges, glm::vec3(x, 100.f, z), glm::vec3(0.f, -1.f, 0.f),
                                 kForever)
           .length;
}

} // namespace

TEST_CASE("FogVolumeGather: each shape becomes its kind of volume, scaled with its entity")
{
    ECS::Scene scene;
    const ECS::Entity box = Place(scene, glm::dvec3(0.0, 0.0, -30.0), glm::vec3(2.f, 1.f, 1.f));
    REQUIRE(scene.Add(box, FogBox{.size = glm::vec3(10.f, 4.f, 10.f), .edgeSoftness = 0.f, .falloff = 2.f}) != nullptr);
    const ECS::Entity sphere = Place(scene, glm::dvec3(5.0, 0.0, -30.0), glm::vec3(1.f, 2.f, 1.f));
    REQUIRE(scene.Add(sphere, FogSphere{.radius = 3.f, .edgeSoftness = 0.f, .pile = 0.5f}) != nullptr);

    const Render::FogVolumeFrame frame = Gather(scene);
    REQUIRE(frame.volumes.size() == 2u);
    CHECK(frame.volumes[0].info.x == static_cast<uint32_t>(Render::FogVolumeKind::Box));
    CHECK(glm::vec3(frame.volumes[0].extents) == glm::vec3(10.f, 2.f, 5.f));
    CHECK(frame.volumes[0].profile.x == 2.f);
    CHECK(frame.volumes[1].info.x == static_cast<uint32_t>(Render::FogVolumeKind::Ellipsoid));
    CHECK(glm::vec3(frame.volumes[1].extents) == glm::vec3(3.f, 6.f, 3.f));
    CHECK(frame.volumes[1].profile.y == 0.5f);
    // Down through the box's 4 m, thinning above its floor: 2 (1 - e^-2).
    CHECK(Down(frame, 0, 9.f, -30.f) == doctest::Approx(2.f * (1.f - std::exp(-2.f))).epsilon(5.0e-3));
    CHECK(Down(frame, 0, 11.f, -30.f) == 0.f);
    CHECK(Down(frame, 1, 5.f, -30.f) == doctest::Approx(12.f));

    // Lists in the frame's screen squares.
    CHECK(frame.Lanes() == glm::uvec4(2u, frame.grid.columns, frame.grid.rows, frame.grid.tilePixels));
    CHECK(frame.tiles.size() > frame.grid.columns * frame.grid.rows * 2u);
}

TEST_CASE("FogVolumeGather: an outline fills its shape from its entity's origin up to its height")
{
    ECS::Scene scene;
    // Off to one side of its entity's origin, so placing the volume's frame
    // over the outline's bounds is what puts the fog in the right place.
    const std::vector<glm::vec2> square = {{10.f, -2.f}, {14.f, -2.f}, {14.f, 2.f}, {10.f, 2.f}};
    const ECS::Entity entity = Place(scene, glm::dvec3(0.0, -5.0, -40.0));
    REQUIRE(scene.Add(entity, FogOutline{.outline = Polygon(square), .height = 3.f, .edgeSoftness = 0.f}) != nullptr);

    const Render::FogVolumeFrame frame = Gather(scene);
    REQUIRE(frame.volumes.size() == 1u);
    CHECK(frame.volumes[0].info.x == static_cast<uint32_t>(Render::FogVolumeKind::Outline));
    CHECK(frame.volumes[0].info.w == 4u);
    CHECK(Down(frame, 0, 12.f, -40.f) == doctest::Approx(3.f));
    CHECK(Down(frame, 0, 0.f, -40.f) == 0.f);
    // Its floor is the entity's origin: a line stopping one metre above it
    // passes through two of the three metres.
    const Render::FogSpan stopped = Render::FogVolumeSpan(frame.volumes[0], frame.edges, glm::vec3(12.f, 100.f, -40.f),
                                                          glm::vec3(0.f, -1.f, 0.f), 104.f);
    CHECK(stopped.length == doctest::Approx(2.f));
}

TEST_CASE("FogVolumeGather: an outline is scaled across with its entity")
{
    ECS::Scene scene;
    const std::vector<glm::vec2> square = {{-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f}, {-1.f, 1.f}};
    const ECS::Entity entity = Place(scene, glm::dvec3(0.0, 0.0, -40.0), glm::vec3(2.f, 1.f, 1.f));
    REQUIRE(scene.Add(entity, FogOutline{.outline = Polygon(square), .height = 3.f, .edgeSoftness = 0.f}) != nullptr);

    const Render::FogVolumeFrame frame = Gather(scene);
    REQUIRE(frame.volumes.size() == 1u);
    CHECK(Down(frame, 0, 1.5f, -40.f) == doctest::Approx(3.f));
    CHECK(Down(frame, 0, 2.5f, -40.f) == 0.f);
    CHECK(Down(frame, 0, 0.f, -40.5f) == doctest::Approx(3.f));
    CHECK(Down(frame, 0, 0.f, -41.5f) == 0.f);
}

TEST_CASE("FogVolumeGather: volumes off screen, without thickness or without size are left out")
{
    ECS::Scene scene;
    const ECS::Entity behind = Place(scene, glm::dvec3(0.0, 0.0, 40.0));
    REQUIRE(scene.Add(behind, FogBox{}) != nullptr);
    const ECS::Entity clear = Place(scene, glm::dvec3(0.0, 0.0, -40.0));
    REQUIRE(scene.Add(clear, FogSphere{.visibility = 0.f}) != nullptr);
    const ECS::Entity flat = Place(scene, glm::dvec3(0.0, 0.0, -40.0));
    REQUIRE(scene.Add(flat, FogBox{.size = glm::vec3(10.f, 0.f, 10.f)}) != nullptr);
    const ECS::Entity line = Place(scene, glm::dvec3(0.0, 0.0, -40.0));
    const std::vector<glm::vec2> collinear = {{0.f, 0.f}, {1.f, 0.f}, {2.f, 0.f}};
    REQUIRE(scene.Add(line, FogOutline{.outline = Polygon(collinear)}) != nullptr);

    const Render::FogVolumeFrame frame = Gather(scene);
    CHECK(frame.volumes.empty());
    CHECK(frame.edges.empty());
    CHECK(frame.Lanes().x == 0u);
}
