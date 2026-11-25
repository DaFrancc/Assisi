/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/ScenePasses.hpp>

#include <limits>

using namespace Assisi;
using Runtime::Camera;
using Runtime::CameraFeature;
using Runtime::Passes::ResolveViewFeatures;
using Runtime::Passes::SceneSettings;
using Runtime::Passes::ViewFeatures;

namespace
{
/// Graphics settings with every feature a camera can switch turned on.
SceneSettings EverythingOn()
{
    SceneSettings settings;
    settings.shadows.sun.enabled = true;
    settings.shadows.local.enabled = true;
    settings.ssao.enabled = true;
    settings.fogEnabled = true;
    settings.waterEnabled = true;
    return settings;
}

/// A camera drawing everything but @p feature.
Camera CameraWithout(CameraFeature feature)
{
    Camera camera;
    camera.features = camera.features.Without(feature);
    return camera;
}
} // namespace

TEST_CASE("A camera draws everything the settings allow until a switch turns a feature off")
{
    const ViewFeatures all = ResolveViewFeatures(EverythingOn(), Camera{});
    CHECK(all.sunShadows);
    CHECK(all.localShadows);
    CHECK(all.ambientOcclusion);
    CHECK(all.sky);
    CHECK(all.fog);
    CHECK(all.water);
    CHECK(all.seeThrough);

    const ViewFeatures unshadowed = ResolveViewFeatures(EverythingOn(), CameraWithout(CameraFeature::Shadows));
    CHECK_FALSE(unshadowed.sunShadows);
    CHECK_FALSE(unshadowed.localShadows);
    CHECK_FALSE(unshadowed.cloudShadows);
    CHECK(unshadowed.ambientOcclusion);
    CHECK_FALSE(ResolveViewFeatures(EverythingOn(), CameraWithout(CameraFeature::AmbientOcclusion)).ambientOcclusion);
    CHECK_FALSE(ResolveViewFeatures(EverythingOn(), CameraWithout(CameraFeature::Sky)).sky);
    CHECK_FALSE(ResolveViewFeatures(EverythingOn(), CameraWithout(CameraFeature::Fog)).fog);
    CHECK_FALSE(ResolveViewFeatures(EverythingOn(), CameraWithout(CameraFeature::Water)).water);
    CHECK_FALSE(ResolveViewFeatures(EverythingOn(), CameraWithout(CameraFeature::SeeThrough)).seeThrough);
}

TEST_CASE("A camera never draws a feature the settings turned off")
{
    SceneSettings settings = EverythingOn();
    settings.shadows.sun.enabled = false;
    settings.ssao.enabled = false;
    settings.fogEnabled = false;
    settings.waterEnabled = false;
    const ViewFeatures features = ResolveViewFeatures(settings, Camera{});
    CHECK_FALSE(features.sunShadows);
    CHECK(features.localShadows);
    // The clouds' shadow has its own setting, so the sun's leaves it on.
    CHECK(features.cloudShadows);
    CHECK_FALSE(features.ambientOcclusion);
    CHECK_FALSE(features.fog);
    CHECK_FALSE(features.water);
}

TEST_CASE("A camera's level-of-detail bias that is not a positive number counts as 1")
{
    Camera camera;
    camera.lodBias = 0.5f;
    CHECK(ResolveViewFeatures(EverythingOn(), camera).lodBias == doctest::Approx(0.5f));
    camera.lodBias = 0.f;
    CHECK(ResolveViewFeatures(EverythingOn(), camera).lodBias == doctest::Approx(1.f));
    camera.lodBias = -2.f;
    CHECK(ResolveViewFeatures(EverythingOn(), camera).lodBias == doctest::Approx(1.f));
    camera.lodBias = std::numeric_limits<float>::quiet_NaN();
    CHECK(ResolveViewFeatures(EverythingOn(), camera).lodBias == doctest::Approx(1.f));
}
