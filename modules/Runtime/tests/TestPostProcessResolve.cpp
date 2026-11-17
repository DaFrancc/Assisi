/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPostProcessResolve.cpp
/// @brief The scene's PostProcessVolumes blend into one frame's settings: the
/// level's under every box, boxes fading in across their blend distance, the
/// higher priority and then the smaller box on top, and only the settings a
/// volume switches on.

#include <doctest/doctest.h>

#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/PostProcessSettings.hpp>
#include <Assisi/Runtime/PostProcessResolve.hpp>
#include <Assisi/Runtime/PostProcessVolume.hpp>

#include <cmath>

using namespace Assisi;
using Assisi::ECS::Transform;
using Assisi::Runtime::ExposureMode;
using Assisi::Runtime::PostProcessExtent;
using Assisi::Runtime::PostProcessVolume;

namespace
{

PostProcessVolume LevelContrast(float contrast, int32_t priority = 0)
{
    return PostProcessVolume{.extent = PostProcessExtent::Level,
                             .priority = priority,
                             .overrideContrast = true,
                             .contrast = contrast};
}

PostProcessVolume BoxContrast(float contrast, const glm::vec3 &size, int32_t priority = 0)
{
    return PostProcessVolume{.extent = PostProcessExtent::Box,
                             .size = size,
                             .blendDistance = 2.0f,
                             .priority = priority,
                             .overrideContrast = true,
                             .contrast = contrast};
}

void Place(ECS::Scene &scene, const PostProcessVolume &volume, const Transform &transform = {})
{
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, transform) != nullptr);
    REQUIRE(scene.Add(entity, volume) != nullptr);
}

Runtime::PostProcessResolution Resolve(ECS::Scene &scene, const glm::dvec3 &camera)
{
    (void)ECS::PropagateTransforms(scene, 0u);
    return Runtime::ResolvePostProcess(scene, camera);
}

} // namespace

TEST_CASE("A scene with no volumes is finished with the defaults")
{
    ECS::Scene scene;
    const Runtime::PostProcessResolution resolution = Resolve(scene, glm::dvec3(0.0));
    CHECK(resolution.settings == Render::PostProcessSettings{});
    CHECK(resolution.levelVolumes == 0u);
}

TEST_CASE("A volume changes only the settings it switches on")
{
    ECS::Scene scene;
    PostProcessVolume volume = LevelContrast(2.0f);
    volume.saturation = 0.0f; // not switched on
    Place(scene, volume);
    const Render::PostProcessSettings settings = Resolve(scene, glm::dvec3(100.0)).settings;
    CHECK(settings.contrast == doctest::Approx(2.0f));
    CHECK(settings.saturation == doctest::Approx(Render::PostProcessSettings{}.saturation));
    CHECK(settings.exposure == Render::ExposureSettings{});
}

TEST_CASE("A box counts fully inside, fades across its blend distance, and not at all past it")
{
    CHECK(Runtime::PostProcessVolumeWeight(0.0f, 2.0f) == 1.0f);
    CHECK(Runtime::PostProcessVolumeWeight(1.0f, 2.0f) == doctest::Approx(0.5f));
    CHECK(Runtime::PostProcessVolumeWeight(2.5f, 2.0f) == 0.0f);
    CHECK(Runtime::PostProcessVolumeWeight(0.5f, 0.0f) == 0.0f);

    // A 10 m box turned a quarter turn about y and stretched twice as long
    // along its own x, which then lies along the world's z.
    ECS::Scene scene;
    const glm::quat quarter = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    Place(scene, BoxContrast(2.0f, glm::vec3(10.0f)),
          Transform{.position = glm::dvec3(50.0, 0.0, 0.0), .rotation = quarter, .scale = glm::vec3(2.0f, 1.0f, 1.0f)});
    const float neutral = Render::PostProcessSettings{}.contrast;

    CHECK(Resolve(scene, glm::dvec3(50.0, 0.0, 0.0)).settings.contrast == doctest::Approx(2.0f));
    // Its long side reaches 10 m along z, so 9 m is inside.
    CHECK(Resolve(scene, glm::dvec3(50.0, 0.0, 9.0)).settings.contrast == doctest::Approx(2.0f));
    // 1 m past the end of the long side: half way through the blend.
    CHECK(Resolve(scene, glm::dvec3(50.0, 0.0, 11.0)).settings.contrast ==
          doctest::Approx(0.5f * (neutral + 2.0f)));
    // Its short side ends 5 m along x; 1 m past is half way too.
    CHECK(Resolve(scene, glm::dvec3(56.0, 0.0, 0.0)).settings.contrast ==
          doctest::Approx(0.5f * (neutral + 2.0f)));
    CHECK(Resolve(scene, glm::dvec3(50.0, 0.0, 13.0)).settings.contrast == doctest::Approx(neutral));
}

TEST_CASE("Boxes go over the level's volume, the higher priority on top, then the smaller")
{
    ECS::Scene scene;
    // A whole-level volume of the highest priority still goes under the boxes.
    Place(scene, LevelContrast(0.6f, 100));
    Place(scene, BoxContrast(1.0f, glm::vec3(40.0f), 1));
    Place(scene, BoxContrast(1.5f, glm::vec3(20.0f), 0));
    // Inside both boxes: the one of priority 1 wins, though it is the larger.
    CHECK(Resolve(scene, glm::dvec3(0.0)).settings.contrast == doctest::Approx(1.0f));
    // Outside both and past their blend: the level's.
    CHECK(Resolve(scene, glm::dvec3(100.0, 0.0, 0.0)).settings.contrast == doctest::Approx(0.6f));

    // At equal priority the smaller box, the more particular place, wins:
    // a room inside a cave inside a valley.
    ECS::Scene nested;
    Place(nested, BoxContrast(0.8f, glm::vec3(10.0f)));
    Place(nested, BoxContrast(2.2f, glm::vec3(100.0f)));
    Place(nested, BoxContrast(1.2f, glm::vec3(50.0f)));
    CHECK(Resolve(nested, glm::dvec3(0.0)).settings.contrast == doctest::Approx(0.8f));
    CHECK(Resolve(nested, glm::dvec3(20.0, 0.0, 0.0)).settings.contrast == doctest::Approx(1.2f));
    CHECK(Resolve(nested, glm::dvec3(40.0, 0.0, 0.0)).settings.contrast == doctest::Approx(2.2f));
}

TEST_CASE("Of two volumes covering the level, the higher priority wins and both are counted")
{
    ECS::Scene scene;
    Place(scene, LevelContrast(2.0f, 5));
    Place(scene, LevelContrast(0.7f, 1));
    const Runtime::PostProcessResolution resolution = Resolve(scene, glm::dvec3(0.0));
    CHECK(resolution.settings.contrast == doctest::Approx(2.0f));
    CHECK(resolution.levelVolumes == 2u);
}

TEST_CASE("A box with no size covers nothing")
{
    ECS::Scene scene;
    Place(scene, BoxContrast(2.0f, glm::vec3(10.0f, 0.0f, 10.0f)));
    Place(scene, BoxContrast(2.0f, glm::vec3(10.0f)), Transform{.scale = glm::vec3(0.0f, 1.0f, 1.0f)});
    CHECK(Resolve(scene, glm::dvec3(0.0)).settings.contrast == doctest::Approx(Render::PostProcessSettings{}.contrast));
}

TEST_CASE("Blending a manual exposure over an automatic one mixes the two by the weight")
{
    const PostProcessVolume automatic{.overrideExposure = true,
                                      .exposureMode = ExposureMode::Automatic,
                                      .minExposureStops = -2.0f,
                                      .maxExposureStops = 4.0f,
                                      .exposureCompensation = 1.0f,
                                      .adaptationSpeed = 3.0f};
    const Render::PostProcessSettings level =
        Runtime::BlendPostProcessVolume(Render::PostProcessSettings{}, automatic, 1.0f);
    CHECK(level.exposure.automatic == doctest::Approx(1.0f));
    CHECK(level.exposure.minStops == doctest::Approx(-2.0f));
    CHECK(level.exposure.maxStops == doctest::Approx(4.0f));
    CHECK(level.exposure.compensationStops == doctest::Approx(1.0f));
    CHECK(level.exposure.adaptationRate == doctest::Approx(3.0f));

    const PostProcessVolume manual{
        .overrideExposure = true, .exposureMode = ExposureMode::Manual, .exposureStops = 2.0f};
    const Render::PostProcessSettings half = Runtime::BlendPostProcessVolume(level, manual, 0.5f);
    CHECK(half.exposure.automatic == doctest::Approx(0.5f));
    CHECK(half.exposure.manualStops == doctest::Approx(1.0f));
    // A manual volume leaves the automatic range to the volumes under it.
    CHECK(half.exposure.maxStops == doctest::Approx(4.0f));
}

TEST_CASE("White balance blends evenly in colour, not in kelvin")
{
    const PostProcessVolume warm{.overrideWhiteBalance = true, .whiteBalanceKelvin = 3250.0f, .whiteBalanceTint = 0.5f};
    const Render::PostProcessSettings half =
        Runtime::BlendPostProcessVolume(Render::PostProcessSettings{}, warm, 0.5f);
    // Half way between 6500 K and 3250 K in reciprocal kelvin is 4333 K.
    CHECK(half.whiteBalanceKelvin == doctest::Approx(4333.3f).epsilon(0.001));
    CHECK(half.whiteBalanceTint == doctest::Approx(0.25f));
}

TEST_CASE("Bloom's strength and spread come from the volume when it switches them on")
{
    const PostProcessVolume glow{.overrideBloom = true, .bloomIntensity = 0.2f, .bloomScatter = 0.3f};
    const Render::PostProcessSettings settings =
        Runtime::BlendPostProcessVolume(Render::PostProcessSettings{}, glow, 1.0f);
    CHECK(settings.bloomIntensity == doctest::Approx(0.2f));
    CHECK(settings.bloomScatter == doctest::Approx(0.3f));
}
