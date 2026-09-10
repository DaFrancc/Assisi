/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/glm.hpp>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/LightComponents.hpp>
#include <Assisi/Runtime/LightingSystem.hpp>
#include <Assisi/Runtime/SkyComponents.hpp>
#include <Assisi/Runtime/SkyResolve.hpp>
#include <Assisi/Runtime/TimeOfDay.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>

using Assisi::Runtime::LightingSystem;

namespace
{
// Component-wise compare; the direction is normalized so an exact match is not
// expected, only agreement to float tolerance.
bool Approx3(const glm::vec3 &a, const glm::vec3 &b)
{
    return a.x == doctest::Approx(b.x) && a.y == doctest::Approx(b.y) && a.z == doctest::Approx(b.z);
}
} // namespace

// A spot light is aimed by turning it: the beam is its Transform's local -Y
// carried through the entity's world matrix, so a light parented to something
// that turns aims with it — the same rule its position follows.
TEST_CASE("SpotWorldDirection reads a spot's aim off its world matrix")
{
    using Assisi::Runtime::SpotWorldDirection;
    const glm::vec3 down{0.f, -1.f, 0.f};

    SUBCASE("an unturned spot shines straight down")
    {
        CHECK(Approx3(SpotWorldDirection(glm::mat4(1.f)), down));
    }

    SUBCASE("a quarter turn about +X tips the beam from down to -Z")
    {
        const glm::mat4 pitch = glm::rotate(glm::mat4(1.f), glm::radians(90.f), glm::vec3(1.f, 0.f, 0.f));
        CHECK(Approx3(SpotWorldDirection(pitch), glm::vec3(0.f, 0.f, -1.f)));
    }

    SUBCASE("translation alone does not steer the beam")
    {
        const glm::mat4 moved = glm::translate(glm::mat4(1.f), glm::vec3(10.f, -3.f, 7.f));
        CHECK(Approx3(SpotWorldDirection(moved), down));
    }

    SUBCASE("the result is normalized despite scale in the matrix")
    {
        const glm::mat4 scaled = glm::scale(glm::mat4(1.f), glm::vec3(5.f));
        const glm::vec3 out    = SpotWorldDirection(scaled);
        CHECK(glm::length(out) == doctest::Approx(1.f));
        CHECK(Approx3(out, down));
    }

    SUBCASE("a collapsed matrix falls back instead of producing NaN")
    {
        const glm::mat4 flattened = glm::scale(glm::mat4(1.f), glm::vec3(0.f));
        const glm::vec3 out = SpotWorldDirection(flattened);
        CHECK(glm::length(out) == doctest::Approx(1.f));
        CHECK(out.x == out.x); // not NaN
    }
}

// The composition the deleted `direction` field bought, and the one thing that
// had to survive losing it: a spot mounted on something that turns — a vehicle
// headlight, a held torch — aims where the mount faces.
TEST_CASE("a parented spot aims with its parent")
{
    using Assisi::ECS::Transform;
    using Assisi::Runtime::Parent;
    using Assisi::Runtime::SpotLight;
    using Assisi::Runtime::SpotWorldDirection;

    Assisi::ECS::Scene scene;

    // The mount is tipped a quarter turn about +X, which takes -Y to -Z and +Y to +Z.
    const Assisi::ECS::Entity mount = scene.Create();
    REQUIRE(scene.Add(mount, Transform{.rotation = glm::angleAxis(glm::radians(90.f),
                                                                 glm::vec3(1.f, 0.f, 0.f))}) != nullptr);

    // The light is unturned in the mount's space, so all of its aim is inherited.
    const Assisi::ECS::Entity light = scene.Create();
    REQUIRE(scene.Add(light, Transform{.position = {0.f, 2.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(light, SpotLight{}) != nullptr);
    REQUIRE(scene.Add(light, Parent{.parent = mount}) != nullptr);

    (void)Assisi::Runtime::PropagateTransforms(scene, 0u);

    const Transform *world = scene.Get<Transform>(light);
    REQUIRE(world != nullptr);
    CHECK(Approx3(SpotWorldDirection(world->worldMatrix), glm::vec3(0.f, 0.f, -1.f)));
    // Position follows the same matrix, and the two must agree about the turn.
    CHECK(Approx3(glm::vec3(world->worldMatrix[3]), glm::vec3(0.f, 0.f, 2.f)));
}

namespace
{
using namespace Assisi;
using Assisi::Runtime::DirectionalLight;
using Assisi::Runtime::LightingBody;
using Assisi::Runtime::Moon;
using Assisi::Runtime::Skybox;
using Assisi::Runtime::Sun;
using Assisi::Runtime::TimeOfDay;

/// A scene whose one directional light is on the clock, with an atmosphere. The
/// light is authored bright and shadow-casting, because what these cases are
/// about is the authored values NOT reaching the buffer when the clock says
/// nothing is lighting.
ECS::Entity AddClockedSunEntity(ECS::Scene &scene, double hour, bool withMoon)
{
    const ECS::Entity entity = scene.Create();
    (void)scene.Add<DirectionalLight>(
        entity, DirectionalLight{.direction = glm::vec3(0.f, -1.f, 0.f), .intensity = 7.f, .castsShadows = true});
    (void)scene.Add<Skybox>(entity);
    (void)scene.Add<Sun>(entity, Sun{.latitudeDegrees = 45.f});

    TimeOfDay clock;
    clock.hour = hour;
    (void)scene.Add<TimeOfDay>(entity, clock);

    if (withMoon)
    {
        (void)scene.Add<Moon>(entity, Moon{.phaseAtEpoch = 0.5f});
    }
    return entity;
}
} // namespace

TEST_CASE("A night with nothing up uploads a dark sun, not the authored one")
{
    // The defect this exists for: the resolver names the sun's entity whether or
    // not a body is lighting it, and Gather claims the row by that name. When it
    // did not — when "nothing is lighting" was expressed by leaving the entity
    // null — the row went unclaimed and Gather fell back to the AUTHORED aim,
    // which lit the world at full intensity from straight overhead in the middle
    // of the night, and fixed itself the moment either body came back up.
    ECS::Scene scene;
    (void)AddClockedSunEntity(scene, 0.0, /*withMoon=*/false);

    const Runtime::SkyResolution sky = Runtime::ResolveSky(scene);
    REQUIRE(sky.sun.directionToSun.y < 0.f);
    REQUIRE(sky.light.body == LightingBody::None);
    // Named even with nothing lighting. This is the assertion that would have
    // caught it.
    REQUIRE(sky.light.entity != ECS::NullEntity);

    LightingSystem lighting;
    lighting.Gather(scene, &sky.light);

    REQUIRE(lighting.DirLightCount() == 1u);
    const std::span<const Assisi::Runtime::ShadowCaster> flags = lighting.DirLightShadowFlags();
    REQUIRE(flags.size() == 1u);
    // Nothing lights, so nothing shadows either — which is what makes a long
    // polar night cost no cascades at all rather than a full draw every frame.
    CHECK(flags[0] == Assisi::Runtime::ShadowCaster::No);
    CHECK_FALSE(lighting.ShadowCastingSun().has_value());
}

TEST_CASE("The row the clock drives carries whichever body is lighting it")
{
    // Midnight with a full moon up: the row is the MOON's aim, not the sun's and
    // not the authored one.
    ECS::Scene scene;
    (void)AddClockedSunEntity(scene, 0.0, /*withMoon=*/true);

    const Runtime::SkyResolution night = Runtime::ResolveSky(scene);
    REQUIRE(night.light.body == LightingBody::Moon);
    REQUIRE(night.moon.directionToMoon.y > 0.f);

    LightingSystem lighting;
    lighting.Gather(scene, &night.light);
    REQUIRE(lighting.DirLightCount() == 1u);
    REQUIRE(lighting.ShadowCastingSun().has_value());
    CHECK(Approx3(lighting.ShadowCastingSun()->direction, -night.moon.directionToMoon));

    // And by day the same row is the sun's.
    scene.GetMut<TimeOfDay>(ECS::Entity{.index = 0, .generation = 0})->hour = 12.0;
    const Runtime::SkyResolution day = Runtime::ResolveSky(scene);
    REQUIRE(day.light.body == LightingBody::Sun);

    lighting.Gather(scene, &day.light);
    REQUIRE(lighting.ShadowCastingSun().has_value());
    CHECK(Approx3(lighting.ShadowCastingSun()->direction, -day.sun.directionToSun));
}

TEST_CASE("A light the clock does not drive is still gathered as authored")
{
    // The fallback is not dead code: a directional light with no Sun beside it is
    // aimed by hand, and Gather must leave it exactly as authored.
    ECS::Scene scene;
    const glm::vec3 authored = glm::normalize(glm::vec3(1.f, -2.f, 0.5f));
    const ECS::Entity entity = scene.Create();
    (void)scene.Add<DirectionalLight>(
        entity, DirectionalLight{.direction = authored, .intensity = 2.f, .castsShadows = true});

    const Runtime::SkyResolution sky = Runtime::ResolveSky(scene);
    REQUIRE(sky.status == Runtime::SkyStatus::NoSkybox);

    LightingSystem lighting;
    lighting.Gather(scene, &sky.light);
    REQUIRE(lighting.ShadowCastingSun().has_value());
    CHECK(Approx3(lighting.ShadowCastingSun()->direction, authored));
}

