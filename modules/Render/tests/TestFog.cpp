/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Fog.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

using namespace Assisi::Render;

namespace
{
/// A direction from an elevation above the horizon and an azimuth about +Y.
glm::vec3 Dir(float elevationDegrees, float azimuthDegrees)
{
    const float el = glm::radians(elevationDegrees);
    const float az = glm::radians(azimuthDegrees);
    return glm::vec3(std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az));
}

SkySun SunAt(float elevationDegrees, float azimuthDegrees = 0.0f)
{
    return SkySun{.directionToSun = Dir(elevationDegrees, azimuthDegrees), .color = glm::vec3(1.0f), .intensity = 1.0f};
}

/// Far enough that no test about the layers reads the edge fade instead.
constexpr float kNoEdge = 1.0e6f;

/// The optical depth of a straight line through @p layer by brute force: the
/// density at the middle of each of many short steps, summed.
float SummedOpticalDepth(const FogLayer &layer, float length, float fromHeight, float toHeight)
{
    constexpr int32_t kSteps = 20000;
    double depth = 0.0;
    const double step = static_cast<double>(length) / kSteps;
    for (int32_t i = 0; i < kSteps; ++i)
    {
        const double t = (static_cast<double>(i) + 0.5) / kSteps;
        const float height = static_cast<float>(static_cast<double>(fromHeight) +
                                                t * (static_cast<double>(toHeight) - static_cast<double>(fromHeight)));
        depth += static_cast<double>(FogLayerDensityAt(layer, height)) * step;
    }
    return static_cast<float>(depth);
}

bool Near(const glm::vec3 &a, const glm::vec3 &b, float relative)
{
    const float scale = std::max(glm::length(a), glm::length(b));
    return glm::length(a - b) <= relative * scale;
}
} // namespace

TEST_CASE("Fog settings are sanitized into their ranges")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    FogSettings raw;
    raw.color = glm::vec3(nan, -1.0f, 0.5f);
    raw.hazeVisibility = -10.0f;
    raw.hazeFalloff = 0.25f;
    raw.mistVisibility = nan;
    raw.mistHeight = std::numeric_limits<float>::infinity();
    raw.mistFalloff = -5.0f;

    const FogSettings clean = Sanitized(raw);
    CHECK(clean.color.r == doctest::Approx(kDefaultFogColor.r));
    CHECK(clean.color.g == 0.0f);
    CHECK(clean.color.b == doctest::Approx(0.5f));
    CHECK(clean.hazeVisibility == 0.0f);
    // A falloff above zero is at least the minimum; at or below zero it is even.
    CHECK(clean.hazeFalloff == doctest::Approx(kMinFogFalloff));
    CHECK(clean.mistVisibility == 0.0f);
    CHECK(clean.mistHeight == 0.0f);
    CHECK(clean.mistFalloff == 0.0f);

    const FogSettings twice = Sanitized(clean);
    CHECK(twice.hazeFalloff == clean.hazeFalloff);
    CHECK(twice.color == clean.color);
}

TEST_CASE("A fog with no layers hides nothing until the edge fade")
{
    const FogSettings none;
    const float drawDistance = 1000.0f;
    CHECK(FogOpacity(none, 10.0f, 0.0f, 0.0f, drawDistance) == 0.0f);
    CHECK(FogOpacity(none, drawDistance * (1.0f - kFogEdgeFadeFraction) - 1.0f, 0.0f, 0.0f, drawDistance) == 0.0f);
    CHECK(FogOpacity(none, drawDistance * (1.0f - 0.5f * kFogEdgeFadeFraction), 0.0f, 0.0f, drawDistance) > 0.0f);
    CHECK(FogOpacity(none, drawDistance, 0.0f, 0.0f, drawDistance) == doctest::Approx(1.0f));
}

TEST_CASE("The haze hides 99% at its visibility where it is thickest")
{
    FogSettings fog;
    fog.hazeVisibility = 2000.0f;
    CHECK(FogOpacity(fog, 2000.0f, 5.0f, 5.0f, kNoEdge) == doctest::Approx(0.99f).epsilon(1e-4));

    // The same holds inside the full-thickness band of a layer that thins above it.
    fog.hazeHeight = 100.0f;
    fog.hazeFalloff = 50.0f;
    CHECK(FogOpacity(fog, 2000.0f, 20.0f, 20.0f, kNoEdge) == doctest::Approx(0.99f).epsilon(1e-4));

    FogSettings mist;
    mist.mistVisibility = 300.0f;
    CHECK(FogOpacity(mist, 300.0f, 0.0f, 0.0f, kNoEdge) == doctest::Approx(0.99f).epsilon(1e-4));
}

TEST_CASE("Fog never thins with distance")
{
    FogSettings fog;
    fog.hazeVisibility = 3000.0f;
    fog.hazeFalloff = 200.0f;
    fog.mistVisibility = 400.0f;
    fog.mistHeight = -50.0f;
    fog.mistFalloff = 20.0f;

    float previous = 0.0f;
    for (float distance = 0.0f; distance <= 3000.0f; distance += 25.0f)
    {
        // Looking down a slope: the point falls 1 m for every 10 m out.
        const float opacity = FogOpacity(fog, distance, 30.0f, 30.0f - distance * 0.1f, 3000.0f);
        CHECK(opacity >= previous);
        previous = opacity;
    }
}

TEST_CASE("Everything is fully fogged at the draw distance, however thin the fog is there")
{
    FogSettings fog;
    fog.hazeVisibility = 4000.0f;
    fog.hazeFalloff = 30.0f;
    // A camera and a peak a kilometre above where the haze thins out: the layer
    // alone barely touches the line between them.
    const float alone = FogOpacity(fog, 4000.0f, 1000.0f, 1100.0f, kNoEdge);
    CHECK(alone < 0.1f);
    CHECK(FogOpacity(fog, 4000.0f, 1000.0f, 1100.0f, 4000.0f) == doctest::Approx(1.0f));
    CHECK(FogOpacity(fog, 5000.0f, 1000.0f, 1100.0f, 4000.0f) == doctest::Approx(1.0f));
}

TEST_CASE("A layer is full below its height and thins by e for every falloff above it")
{
    const FogLayer layer{.density = 0.01f, .height = 50.0f, .falloff = 20.0f};
    CHECK(FogLayerDensityAt(layer, -300.0f) == doctest::Approx(0.01f));
    CHECK(FogLayerDensityAt(layer, 50.0f) == doctest::Approx(0.01f));
    CHECK(FogLayerDensityAt(layer, 70.0f) == doctest::Approx(0.01f / std::exp(1.0f)));
    CHECK(FogLayerDensityAt(layer, 90.0f) == doctest::Approx(0.01f / std::exp(2.0f)));

    const FogLayer even{.density = 0.01f, .height = 50.0f, .falloff = 0.0f};
    CHECK(FogLayerDensityAt(even, 5000.0f) == doctest::Approx(0.01f));
}

TEST_CASE("A layer's optical depth is the integral of its density along the line")
{
    const FogLayer layer{.density = 0.004f, .height = 40.0f, .falloff = 25.0f};
    struct Line
    {
        float length;
        float from;
        float to;
    };
    // Rising, falling, wholly below, wholly above, and crossing the height both ways.
    const Line lines[] = {
        {500.0f, 0.0f, 20.0f},   {500.0f, 20.0f, 0.0f},   {800.0f, 10.0f, 200.0f}, {800.0f, 200.0f, 10.0f},
        {300.0f, 100.0f, 160.0f}, {1200.0f, -80.0f, -20.0f}, {2000.0f, 300.0f, 35.0f},
    };
    for (const Line &line : lines)
    {
        CAPTURE(line.from);
        CAPTURE(line.to);
        CHECK(FogLayerOpticalDepth(layer, line.length, line.from, line.to) ==
              doctest::Approx(SummedOpticalDepth(layer, line.length, line.from, line.to)).epsilon(1e-3));
    }
}

TEST_CASE("A nearly level line and a level one collect the same depth")
{
    const FogLayer layer{.density = 0.004f, .height = 40.0f, .falloff = 25.0f};
    const float level = FogLayerOpticalDepth(layer, 1000.0f, 60.0f, 60.0f);
    const float justUnder = FogLayerOpticalDepth(layer, 1000.0f, 60.0f, 60.0f + 0.5f * kFogLevelSpan);
    const float justOver = FogLayerOpticalDepth(layer, 1000.0f, 60.0f, 60.0f + 2.0f * kFogLevelSpan);
    CHECK(justUnder == doctest::Approx(level).epsilon(1e-3));
    CHECK(justOver == doctest::Approx(level).epsilon(1e-3));

    // A long falloff over a short rise: the two exponentials agree to every bit
    // a float has, and the answer must still be the level one.
    const FogLayer gentle{.density = 0.001f, .height = 0.0f, .falloff = kMaxFogFalloff};
    CHECK(FogLayerOpticalDepth(gentle, 1000.0f, 10.0f, 11.0f) ==
          doctest::Approx(SummedOpticalDepth(gentle, 1000.0f, 10.0f, 11.0f)).epsilon(1e-3));
}

TEST_CASE("The draw distance never reaches past the haze")
{
    FogSettings fog;
    CHECK(FogDrawDistance(500.0f, fog) == 500.0f);
    fog.hazeVisibility = 2000.0f;
    CHECK(FogDrawDistance(5000.0f, fog) == 2000.0f);
    CHECK(FogDrawDistance(200.0f, fog) == 200.0f);
    // The mist is local: it never limits how far the camera draws.
    fog.hazeVisibility = 0.0f;
    fog.mistVisibility = 100.0f;
    CHECK(FogDrawDistance(5000.0f, fog) == 5000.0f);
}

TEST_CASE("Above the horizon the fog is the colour of the sky behind it, without the disks")
{
    const SkySun sun = SunAt(25.0f, 40.0f);
    SkySettings sky;
    for (float elevation = 2.0f; elevation <= 90.0f; elevation += 11.0f)
    {
        for (float azimuth = 0.0f; azimuth < 360.0f; azimuth += 30.0f)
        {
            SkySettings noDisk = sky;
            noDisk.sunDiskIntensity = 0.0f;
            const glm::vec3 direction = Dir(elevation, azimuth);
            CHECK(Near(FogRadiance(direction, sun, SkyMoon{}, sky), SkyRadiance(direction, sun, noDisk), 1e-5f));
        }
    }

    // Looking straight at the sun the sky holds its disk and the fog does not.
    const glm::vec3 atSun = sun.directionToSun;
    CHECK(glm::length(FogRadiance(atSun, sun, SkyMoon{}, sky)) < 0.5f * glm::length(SkyRadiance(atSun, sun, sky)));
}

TEST_CASE("Below the horizon the fog is the horizon's colour at the same bearing, with no step across it")
{
    const SkySun sun = SunAt(8.0f, 120.0f);
    const SkySettings sky;
    const float bandTop = glm::degrees(std::asin(kHorizonSoftness));
    for (float azimuth = 0.0f; azimuth < 360.0f; azimuth += 20.0f)
    {
        const glm::vec3 atBandTop = FogRadiance(Dir(bandTop, azimuth), sun, SkyMoon{}, sky);
        CHECK(Near(FogRadiance(Dir(-35.0f, azimuth), sun, SkyMoon{}, sky), atBandTop, 1e-4f));
        CHECK(Near(FogRadiance(Dir(0.0f, azimuth), sun, SkyMoon{}, sky), atBandTop, 1e-4f));

        // Above the band the fog follows the sky, which changes fast near the
        // horizon. No step means the change shrinks with the angle: a tenth of
        // the angle, about a tenth of the change.
        const float wide = glm::length(FogRadiance(Dir(bandTop + 0.01f, azimuth), sun, SkyMoon{}, sky) - atBandTop);
        const float narrow =
            glm::length(FogRadiance(Dir(bandTop + 0.001f, azimuth), sun, SkyMoon{}, sky) - atBandTop);
        CHECK(narrow <= 0.2f * wide + 1e-6f * glm::length(atBandTop));
    }
    // Straight down has no bearing, and still answers a horizon colour.
    const glm::vec3 down = FogRadiance(glm::vec3(0.0f, -1.0f, 0.0f), sun, SkyMoon{}, sky);
    CHECK(glm::length(down) > 0.0f);
    CHECK(std::isfinite(down.r));
}

TEST_CASE("The fog's constants put each layer in its slot and say where the colour comes from")
{
    FogSettings fog;
    fog.hazeVisibility = 2000.0f;
    fog.hazeHeight = 10.0f;
    fog.hazeFalloff = 300.0f;
    fog.mistVisibility = 250.0f;
    fog.mistHeight = -40.0f;
    fog.mistFalloff = 15.0f;
    fog.color = glm::vec3(0.1f, 0.2f, 0.3f);

    const SkyScatterConstants scatter = MakeSkyScatterConstants(SunAt(30.0f), SkyMoon{}, SkySettings{});
    const FogConstants withSky = MakeFogConstants(fog, 12.0f, 1800.0f, &scatter);
    const glm::vec4 &haze = withSky.layers[static_cast<std::size_t>(FogLayerSlot::Haze)];
    const glm::vec4 &mist = withSky.layers[static_cast<std::size_t>(FogLayerSlot::Mist)];
    CHECK(haze.x == doctest::Approx(FogDensityFor(2000.0f)));
    CHECK(haze.y == doctest::Approx(10.0f));
    CHECK(haze.z == doctest::Approx(300.0f));
    CHECK(mist.x == doctest::Approx(FogDensityFor(250.0f)));
    CHECK(mist.y == doctest::Approx(-40.0f));
    CHECK(mist.z == doctest::Approx(15.0f));
    CHECK(withSky.view.x == 1.0f);
    CHECK(withSky.view.y == doctest::Approx(12.0f));
    CHECK(withSky.view.z == doctest::Approx(1800.0f));
    CHECK(withSky.view.w == doctest::Approx(1800.0f * (1.0f - kFogEdgeFadeFraction)));
    CHECK(withSky.color.w == 1.0f);
    CHECK(withSky.scatter.sunDirection == scatter.sunDirection);

    const FogConstants noSky = MakeFogConstants(fog, 12.0f, 1800.0f, nullptr);
    CHECK(noSky.color.w == 0.0f);
    CHECK(glm::vec3(noSky.color) == glm::vec3(0.1f, 0.2f, 0.3f));
}
