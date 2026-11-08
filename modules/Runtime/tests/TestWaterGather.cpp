/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestWaterGather.cpp
/// @brief The scene's Oceans and Lakes become the frame's bodies of water:
/// placed, turned and scaled with their entities, each lake's fetch its width
/// along the wind, and a body left out when none of it is in view.

#include <doctest/doctest.h>

#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Frustum.hpp>
#include <Assisi/Render/Water.hpp>
#include <Assisi/Runtime/WaterComponents.hpp>
#include <Assisi/Runtime/WaterGather.hpp>

#include <cmath>

using namespace Assisi;
using Assisi::ECS::Transform;
using Assisi::Runtime::Lake;
using Assisi::Runtime::Ocean;

namespace
{

ECS::Entity Place(ECS::Scene &scene, const Transform &transform)
{
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, transform) != nullptr);
    return entity;
}

Render::WaterFrame Gather(ECS::Scene &scene, const Runtime::WaterGatherView &view)
{
    (void)ECS::PropagateTransforms(scene, 0u);
    Render::WaterFrame frame;
    Runtime::GatherWater(scene, view, frame);
    return frame;
}

Runtime::WaterGatherView From(glm::dvec3 camera)
{
    return Runtime::WaterGatherView{.camera = camera, .drawDistance = 5000.0f};
}

float WindHeight(const Render::WaterWaves &waves)
{
    return Render::SignificantWaveHeight(Render::SpectrumOf(waves.wind));
}

} // namespace

TEST_CASE("An Ocean is endless water at its entity's height, with its wind and swell")
{
    ECS::Scene scene;
    const ECS::Entity sea = Place(scene, Transform{.position = glm::dvec3(120.0, -2.5, 40.0)});
    REQUIRE(scene.Add(sea, Ocean{.windSpeed = 11.0f, .swellHeight = 1.5f, .swellPeriod = 12.0f}) != nullptr);

    const Render::WaterFrame frame = Gather(scene, From(glm::dvec3(0.0, 3.0, 0.0)));
    REQUIRE(frame.bodies.size() == 1u);
    const Render::WaterBody &body = frame.bodies.front();
    CHECK(body.region.origin.y == doctest::Approx(-2.5));
    // The world's origin, whatever the entity's: the patches keep the world's grid.
    CHECK(body.region.origin.x == 0.0);
    CHECK(body.region.origin.z == 0.0);
    CHECK(body.region.halfExtent.x == 0.0f);
    CHECK(body.nodeCount > 0u);
    CHECK(body.firstNode + body.nodeCount == frame.nodes.size());
    CHECK(body.waves.wind.windSpeed == 11.0f);
    CHECK(body.waves.wind.fetch == Render::kOceanFetch);
    CHECK(body.waves.swell.height == 1.5f);
    CHECK(body.waves.swell.period == 12.0f);
    CHECK(body.significantHeight > 1.5f);
    CHECK(body.waveReach > body.significantHeight);

    // The same body from frame to frame.
    const Render::WaterFrame again = Gather(scene, From(glm::dvec3(10.0, 3.0, 0.0)));
    REQUIRE(again.bodies.size() == 1u);
    CHECK(again.bodies.front().key == body.key);
}

TEST_CASE("A Lake turns and scales with its entity")
{
    ECS::Scene scene;
    const float turn = glm::radians(90.0f);
    const ECS::Entity entity = Place(scene, Transform{.position = glm::dvec3(50.0, 140.0, -20.0),
                                                      .rotation = glm::angleAxis(turn, glm::vec3(0.0f, 1.0f, 0.0f)),
                                                      .scale = glm::vec3(2.0f, 1.0f, 3.0f)});
    REQUIRE(scene.Add(entity, Lake{.size = glm::vec2(30.0f, 10.0f)}) != nullptr);

    const Render::WaterFrame frame = Gather(scene, From(glm::dvec3(50.0, 145.0, -20.0)));
    REQUIRE(frame.bodies.size() == 1u);
    const Render::WaterBody &body = frame.bodies.front();
    CHECK(body.region.origin.x == doctest::Approx(50.0));
    CHECK(body.region.origin.y == doctest::Approx(140.0));
    CHECK(body.region.halfExtent.x == doctest::Approx(30.0f));
    CHECK(body.region.halfExtent.y == doctest::Approx(15.0f));
    // A quarter turn about up takes x to -z.
    CHECK(body.region.axisX.x == doctest::Approx(0.0f));
    CHECK(body.region.axisX.y == doctest::Approx(-1.0f));
}

TEST_CASE("A Lake's fetch is its width along the wind, so a pond's waves stay small")
{
    const Lake long_{.windSpeed = 8.0f, .windBearingDegrees = 90.0f};
    // Its x axis runs east, the way the wind blows.
    const Render::WaterWaves along = Runtime::LakeWaves(long_, glm::vec2(300.0f, 20.0f), glm::vec2(1.0f, 0.0f));
    CHECK(along.wind.fetch == doctest::Approx(300.0f));
    // Turned so its x axis runs north, the wind crosses its narrow width.
    const Render::WaterWaves across = Runtime::LakeWaves(long_, glm::vec2(300.0f, 20.0f), glm::vec2(0.0f, -1.0f));
    CHECK(across.wind.fetch == doctest::Approx(20.0f));
    CHECK(WindHeight(across) < WindHeight(along));

    // A pond under the same wind as a big lake.
    const Render::WaterWaves pond = Runtime::LakeWaves(long_, glm::vec2(8.0f, 8.0f), glm::vec2(1.0f, 0.0f));
    const Render::WaterWaves big = Runtime::LakeWaves(long_, glm::vec2(3000.0f, 3000.0f), glm::vec2(1.0f, 0.0f));
    CHECK(WindHeight(pond) < WindHeight(big) / 10.0f);
    CHECK(pond.swell.height == 0.0f);
}

TEST_CASE("A body none of whose patches is in view is left out")
{
    ECS::Scene scene;
    const ECS::Entity lake = Place(scene, Transform{.position = glm::dvec3(0.0, 0.0, 400.0)});
    REQUIRE(scene.Add(lake, Lake{.size = glm::vec2(20.0f, 20.0f)}) != nullptr);

    // Looking down -z, away from the lake behind the camera.
    const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 5000.0f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const Render::Frustum frustum = Render::Frustum::FromViewProjection(projection * view);
    Runtime::WaterGatherView away = From(glm::dvec3(0.0, 2.0, 0.0));
    away.frustum = &frustum;
    CHECK(Gather(scene, away).bodies.empty());

    // Beyond the draw distance.
    Runtime::WaterGatherView near = From(glm::dvec3(0.0, 2.0, 0.0));
    near.drawDistance = 100.0f;
    CHECK(Gather(scene, near).bodies.empty());

    // In view, and its patches stay over it.
    const Render::WaterFrame seen = Gather(scene, From(glm::dvec3(0.0, 2.0, 0.0)));
    REQUIRE(seen.bodies.size() == 1u);
    CHECK(seen.bodies.front().nodeCount > 0u);

    // No size, no water.
    ECS::Scene empty;
    const ECS::Entity flat = Place(empty, Transform{});
    REQUIRE(empty.Add(flat, Lake{.size = glm::vec2(0.0f, 20.0f)}) != nullptr);
    CHECK(Gather(empty, From(glm::dvec3(0.0, 2.0, 0.0))).bodies.empty());
}
