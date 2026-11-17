/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/CloudQuality.hpp>
#include <Assisi/Render/Clouds.hpp>
#include <Assisi/Render/IndirectLighting.hpp>
#include <Assisi/Render/SkyProbeInputs.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

using namespace Assisi::Render;

namespace
{
/// A direction from an elevation above the horizon and a bearing about +Y.
glm::vec3 Dir(float elevationDegrees, float bearingDegrees)
{
    const float el = glm::radians(elevationDegrees);
    const float az = glm::radians(bearingDegrees);
    return glm::vec3(std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az));
}

SkySun SunAt(float elevationDegrees)
{
    return SkySun{.directionToSun = Dir(elevationDegrees, 30.0f), .color = glm::vec3(1.0f), .intensity = 2.0f};
}

constexpr float kTolerance = 1e-4f;
} // namespace

TEST_CASE("the sky map's layout takes a direction to a point and back")
{
    for (float elevation = 0.5f; elevation < 90.0f; elevation += 7.0f)
    {
        for (float bearing = -175.0f; bearing < 180.0f; bearing += 23.0f)
        {
            const glm::vec3 direction = Dir(elevation, bearing);
            const glm::vec2 uv = CloudMapUv(direction);
            CHECK(uv.x >= 0.0f);
            CHECK(uv.x < 1.0f);
            const glm::vec3 back = CloudMapDirection(uv);
            CHECK(glm::length(back - direction) == doctest::Approx(0.0f).epsilon(kTolerance));
        }
    }
}

TEST_CASE("the sky map spans the horizon to straight up, a texel the same angle both ways")
{
    CHECK(CloudMapUv(Dir(0.0f, 40.0f)).y == doctest::Approx(0.0f));
    CHECK(CloudMapUv(glm::vec3(0.0f, 1.0f, 0.0f)).y == doctest::Approx(1.0f));
    // Below the horizon is the bottom row: there are no clouds there.
    CHECK(CloudMapUv(Dir(-30.0f, 40.0f)).y == doctest::Approx(0.0f));
    // Up a quarter turn over a quarter of the width: one degree up is one
    // degree across.
    const float across = CloudMapUv(Dir(10.0f, 11.0f)).x - CloudMapUv(Dir(10.0f, 10.0f)).x;
    const float up = CloudMapUv(Dir(11.0f, 10.0f)).y - CloudMapUv(Dir(10.0f, 10.0f)).y;
    CHECK(up == doctest::Approx(across * static_cast<float>(kCloudMapAspect)).epsilon(kTolerance));
}

TEST_CASE("cloud settings are brought into range")
{
    CloudSettings wild{.coverage = 3.0f,
                       .baseHeight = std::numeric_limits<float>::quiet_NaN(),
                       .thickness = 1.0f,
                       .density = -1.0f,
                       .puffiness = 2.0f,
                       .changeRate = 50.0f,
                       .windSpeed = 1000.0f,
                       .windBearingDegrees = 450.0f};
    const CloudSettings tamed = Sanitized(wild);
    CHECK(tamed.coverage == kMaxCloudCoverage);
    CHECK(tamed.baseHeight == kDefaultCloudBaseHeight);
    CHECK(tamed.thickness == kMinCloudThickness);
    CHECK(tamed.density == kMinCloudDensity);
    CHECK(tamed.puffiness == kMaxCloudPuffiness);
    CHECK(tamed.changeRate == kMaxCloudChangeRate);
    CHECK(tamed.windSpeed == kMaxCloudWindSpeed);
    CHECK(tamed.windBearingDegrees == doctest::Approx(90.0f));
}

TEST_CASE("the clouds move toward the wind's bearing, faster than the wind near the ground")
{
    // Up where the clouds are, the wind outruns the wind 10 m up.
    const double aloft = 5.0 * kCloudWindScale;
    const glm::dvec2 north = CloudWindVelocity(CloudSettings{.windSpeed = 5.0f, .windBearingDegrees = 0.0f});
    CHECK(north.x == doctest::Approx(0.0));
    CHECK(north.y == doctest::Approx(-aloft));
    const glm::dvec2 east = CloudWindVelocity(CloudSettings{.windSpeed = 5.0f, .windBearingDegrees = 90.0f});
    CHECK(east.x == doctest::Approx(aloft));
    CHECK(east.y == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(kCloudWindScale > 1.0);

    // Still air by default.
    CHECK(glm::length(CloudWindVelocity(CloudSettings{})) == 0.0);
}

TEST_CASE("the clouds stay put in the world as the camera moves, and move with the wind")
{
    const CloudSettings settings;
    const CloudMotion still;
    const CloudConstants here = PlaceClouds(settings, glm::dvec3(120.0, 30.0, -45.0), still, 48, 1024);

    // A camera a whole number of repeats of every noise away sees the same
    // clouds: the offsets are where it stands within each repeat.
    const double repeat = 144000.0;
    for (double period : {kCloudShapeNoisePeriod, kCloudDetailNoisePeriod, kCloudAirMassNoisePeriod,
                          kCloudPatchNoisePeriod, kCloudKindNoisePeriod})
    {
        CHECK(repeat / period == doctest::Approx(std::round(repeat / period)));
    }
    const CloudConstants farAway =
        PlaceClouds(settings, glm::dvec3(120.0 + repeat, 30.0, -45.0 - repeat), still, 48, 1024);
    CHECK(farAway.low.shape.x == doctest::Approx(here.low.shape.x).epsilon(kTolerance));
    CHECK(farAway.low.shape.y == doctest::Approx(here.low.shape.y).epsilon(kTolerance));
    CHECK(farAway.low.detail.x == doctest::Approx(here.low.detail.x).epsilon(kTolerance));
    CHECK(farAway.low.weatherA.x == doctest::Approx(here.low.weatherA.x).epsilon(kTolerance));
    CHECK(farAway.low.weatherA.w == doctest::Approx(here.low.weatherA.w).epsilon(kTolerance));
    CHECK(farAway.low.weatherB.x == doctest::Approx(here.low.weatherB.x).epsilon(kTolerance));

    // The wind carrying the low clouds as far as the camera walked leaves the
    // camera where it was among them.
    const glm::dvec2 walked(700.0, -300.0);
    const CloudConstants carried =
        PlaceClouds(settings, glm::dvec3(120.0 + walked.x, 30.0, -45.0 + walked.y), CloudMotion{.drift = walked}, 48,
                    1024);
    CHECK(carried.low.shape.x == doctest::Approx(here.low.shape.x).epsilon(kTolerance));
    CHECK(carried.low.weatherA.y == doctest::Approx(here.low.weatherA.y).epsilon(kTolerance));

    CHECK(here.scales.w == doctest::Approx(30.0f));
    CHECK(here.low.weatherB.z == doctest::Approx(settings.puffiness));
    CHECK(here.low.layer.x == doctest::Approx(settings.baseHeight));
    CHECK(here.low.layer.y == doctest::Approx(settings.baseHeight + settings.thickness));
}

TEST_CASE("the weather map lies around the camera, on whole texels")
{
    // Past the middle of a texel along x, before it along z.
    const glm::dvec3 camera(1500.0, 30.0, -987.6);
    const CloudConstants constants = PlaceClouds(CloudSettings{}, camera, CloudMotion{}, 24, 2048);
    const double texel = kCloudWeatherWidth / kCloudWeatherResolution;
    CHECK(constants.weatherArea.z == doctest::Approx(1.0 / kCloudWeatherWidth));
    CHECK(constants.weatherArea.w == doctest::Approx(texel));

    const double worldX = camera.x + static_cast<double>(constants.weatherArea.x);
    const double worldZ = camera.z + static_cast<double>(constants.weatherArea.y);
    CHECK(std::remainder(worldX, texel) == doctest::Approx(0.0).epsilon(1e-3));
    CHECK(std::remainder(worldZ, texel) == doctest::Approx(0.0).epsilon(1e-3));
    // The nearest texel corner: the camera is never more than half a texel off.
    CHECK(std::abs(constants.weatherArea.x) <= texel * 0.5);
    CHECK(std::abs(constants.weatherArea.y) <= texel * 0.5);
}

TEST_CASE("each seed reads the clouds from somewhere else, and the same seed from the same place")
{
    const glm::dvec3 camera(10.0, 0.0, 20.0);
    const CloudConstants zero = PlaceClouds(CloudSettings{.seed = 0}, camera, CloudMotion{}, 48, 1024);
    const CloudConstants seven = PlaceClouds(CloudSettings{.seed = 7}, camera, CloudMotion{}, 48, 1024);
    const CloudConstants sevenAgain = PlaceClouds(CloudSettings{.seed = 7}, camera, CloudMotion{}, 48, 1024);
    CHECK(seven.low.shape.x == sevenAgain.low.shape.x);
    CHECK(seven.low.weatherA.z == sevenAgain.low.weatherA.z);
    CHECK(seven.low.shape.x != doctest::Approx(zero.low.shape.x).epsilon(kTolerance));
    CHECK(seven.low.weatherA.x != doctest::Approx(zero.low.weatherA.x).epsilon(kTolerance));
    CHECK(seven.low.detail.z != doctest::Approx(zero.low.detail.z).epsilon(kTolerance));

    // Each noise is shifted on its own; one shift for all would read every
    // noise from the same corner and line their features up.
    for (uint32_t lane = 0; lane < std::to_underlying(CloudSeedLane::Count_); ++lane)
    {
        const double fraction = SeedFraction(12345u, static_cast<CloudSeedLane>(lane), 1u);
        CHECK(fraction >= 0.0);
        CHECK(fraction < 1.0);
        for (uint32_t other = 0; other < lane; ++other)
        {
            CHECK(fraction != SeedFraction(12345u, static_cast<CloudSeedLane>(other), 1u));
        }
    }
}

TEST_CASE("the clouds change shape at their change rate, and not at all at zero")
{
    const CloudSettings settings{.changeRate = 1.0f, .windSpeed = 10.0f, .windBearingDegrees = 90.0f};
    CloudMotion motion;
    for (int i = 0; i < 10; ++i)
    {
        motion = AdvanceClouds(motion, settings, 0.1);
    }
    // A second at the clouds' own speed.
    const double carried = 10.0 * kCloudWindScale;
    CHECK(motion.drift.x == doctest::Approx(carried));
    CHECK(motion.churn.x == doctest::Approx(carried));
    CHECK(motion.rise == doctest::Approx(1.0));

    const glm::dvec3 camera(0.0);
    const CloudConstants before = PlaceClouds(settings, camera, CloudMotion{}, 48, 1024);
    const CloudConstants after = PlaceClouds(settings, camera, CloudMotion{.rise = 100.0}, 48, 1024);
    // The noise rises through the clouds, so it is read lower down.
    CHECK(after.low.shape.z != doctest::Approx(before.low.shape.z).epsilon(kTolerance));
    CHECK(after.low.detail.z != doctest::Approx(before.low.detail.z).epsilon(kTolerance));

    CloudMotion frozen;
    frozen = AdvanceClouds(frozen, CloudSettings{.changeRate = 0.0f, .windSpeed = 10.0f}, 0.2);
    CHECK(frozen.rise == 0.0);
    CHECK(glm::length(frozen.churn) == 0.0);
    CHECK(glm::length(frozen.drift) > 0.0);

    // A stall moves the clouds no further than the longest step.
    const CloudMotion stalled = AdvanceClouds(CloudMotion{}, settings, 30.0);
    CHECK(stalled.drift.x == doctest::Approx(carried * kMaxCloudDriftStep));
}

TEST_CASE("a noise offset keeps its precision however far out the camera is")
{
    // A float holds a millimetre near the origin and nothing better than a
    // metre at a thousand kilometres; the offset is taken in double, so the
    // same position within the repeat gives the same offset either way.
    const double near = 123.456;
    const double far = near + kCloudDetailNoisePeriod * 1.0e6;
    CHECK(NoiseOffset(far, kCloudDetailNoisePeriod) == doctest::Approx(NoiseOffset(near, kCloudDetailNoisePeriod)));
    CHECK(NoiseOffset(-near, kCloudDetailNoisePeriod) >= 0.0f);
    CHECK(NoiseOffset(-near, kCloudDetailNoisePeriod) < 1.0f);
}

TEST_CASE("the clouds are lit in the sky's units, by the bodies bright enough to see")
{
    CloudConstants constants;
    const SkySettings sky;
    LightClouds(constants, SunAt(40.0f), SkyMoon{}, sky);
    CHECK(constants.sunDirection.w == 1.0f);
    // No moon at all: it lights nothing and is not marched toward.
    CHECK(constants.moonDirection.w == 0.0f);

    SkySettings brighter = sky;
    brighter.exposure = sky.exposure * 2.0f;
    CloudConstants doubled;
    LightClouds(doubled, SunAt(40.0f), SkyMoon{}, brighter);
    CHECK(doubled.sunBeam.y == doctest::Approx(constants.sunBeam.y * 2.0f));
}

TEST_CASE("cloud cover counts thin clouds for less")
{
    CHECK(CloudCover(CloudSettings{.coverage = 0.0f}) == 0.0f);
    CHECK(CloudCover(CloudSettings{.coverage = 1.0f, .density = 0.0f}) == 0.0f);
    CHECK(CloudCover(CloudSettings{.coverage = 1.0f, .thickness = 2000.0f, .density = 0.08f}) ==
          doctest::Approx(1.0f).epsilon(1e-3));
    CHECK(CloudCover(CloudSettings{.coverage = 0.5f, .thickness = 200.0f, .density = 0.005f}) < 0.2f);
}

TEST_CASE("under an overcast the sky's light is grey, a share of everything a clear day gives")
{
    const SkySun sun = SunAt(50.0f);
    const SkySettings sky;
    const SkyAmbient clear = AmbientFromSky(sun, sky);
    const glm::vec3 direct = DirectLightFromAbove(sun, SkyMoon{}, sky);

    const SkyAmbient none = CloudyAmbient(clear, 0.0f, direct);
    CHECK(glm::vec3(none.sky) == glm::vec3(clear.sky));
    CHECK(glm::vec3(none.ground) == glm::vec3(clear.ground));

    const SkyAmbient overcast = CloudyAmbient(clear, 1.0f, direct);
    CHECK(overcast.sky.r == doctest::Approx(overcast.sky.g));
    CHECK(overcast.sky.g == doctest::Approx(overcast.sky.b));
    // The sky's light and the sun's together, kOvercastTransmission of it.
    const float clearDay = glm::dot(glm::vec3(clear.sky) + direct, kCloudLuminanceWeights);
    CHECK(glm::dot(glm::vec3(overcast.sky), kCloudLuminanceWeights) ==
          doctest::Approx(clearDay * kOvercastTransmission));
    CHECK(glm::dot(direct, kCloudLuminanceWeights) > 0.0f);
    CHECK(overcast.ground.r == doctest::Approx(clear.ground.r * kOvercastTransmission));
}

TEST_CASE("the clouds' shadow is cast from no lower than its floor")
{
    const glm::vec3 high = glm::normalize(glm::vec3(0.3f, 0.8f, -0.2f));
    CHECK(glm::length(CloudShadowLight(high) - high) == doctest::Approx(0.0f));

    const glm::vec3 low = CloudShadowLight(glm::normalize(glm::vec3(1.0f, 0.01f, 0.0f)));
    CHECK(low.y == doctest::Approx(kMinCloudShadowLightHeight));
    CHECK(glm::length(low) == doctest::Approx(1.0f));
    CHECK(low.x > 0.0f);
}

TEST_CASE("the shadow map lies where the light through the camera meets the clouds, on whole texels")
{
    const glm::vec3 light = glm::normalize(glm::vec3(0.0f, 1.0f, 1.0f));
    const glm::dvec3 camera(10.0, 0.0, 20.0);
    const glm::vec2 centre = CloudShadowCentre(camera, light, 1000.0f);
    // A light at 45 degrees reaches the base a thousand metres along z.
    CHECK(centre.y == doctest::Approx(1000.0f).epsilon(0.02));

    const double texel = static_cast<double>(kCloudShadowWidth) / kCloudShadowResolution;
    const double worldX = camera.x + static_cast<double>(centre.x);
    CHECK(std::remainder(worldX, texel) == doctest::Approx(0.0).epsilon(1e-3));

    // A step smaller than a texel leaves the map where it is in the world.
    const glm::dvec3 nudged = camera + glm::dvec3(texel * 0.1, 0.0, 0.0);
    const glm::vec2 kept = CloudShadowCentre(nudged, light, 1000.0f);
    CHECK(nudged.x + static_cast<double>(kept.x) == doctest::Approx(worldX));
}

TEST_CASE("the lit pass's shadow constants say clouds cast, and how wide the map is")
{
    const CloudSettings settings;
    const CloudShadowPlacement placement = PlaceCloudShadow(settings, glm::dvec3(0.0), glm::vec3(0.0f, -1.0f, 0.0f));
    CHECK(placement.light.y == doctest::Approx(1.0f));
    const CloudShadowConstants constants = MakeCloudShadowConstants(settings, placement, 12.0f);
    CHECK(constants.area.w == 1.0f);
    CHECK(constants.area.z == doctest::Approx(1.0f / kCloudShadowWidth));
    CHECK(constants.light.w == doctest::Approx(settings.baseHeight));
    CHECK(constants.outside.y == doctest::Approx(12.0f));
    CHECK(constants.outside.x == doctest::Approx(1.0f - CloudCover(settings)));
}

TEST_CASE("clouds move across the sky by the angle their drift makes overhead")
{
    const glm::dvec2 baked(0.0);
    // A hundred metres under clouds a kilometre and a half up is about 3.8
    // degrees; two metres is under a tenth of one.
    CHECK(CloudsMovedInSky(baked, glm::dvec2(100.0, 0.0), 1500.0f, 0.5f));
    CHECK_FALSE(CloudsMovedInSky(baked, glm::dvec2(2.0, 0.0), 1500.0f, 0.5f));
    // Lower clouds cross more of the sky for the same drift.
    CHECK(CloudsMovedInSky(baked, glm::dvec2(10.0, 0.0), 300.0f, 0.5f));
    CHECK_FALSE(CloudsMovedInSky(baked, glm::dvec2(10.0, 0.0), 3000.0f, 0.5f));
    // Zero tolerance compares exactly.
    CHECK(CloudsMovedInSky(baked, glm::dvec2(1e-6, 0.0), 1500.0f, 0.0f));
    CHECK_FALSE(CloudsMovedInSky(baked, baked, 1500.0f, 0.0f));
}

TEST_CASE("the reflection probe bakes again when the clouds come, go, change or drift")
{
    SkyProbeInputs clear = MakeSkyProbeInputs(SunAt(40.0f), SkyMoon{}, SkySettings{});
    SkyProbeInputs cloudy = clear;
    cloudy.clouds = SkyProbeClouds{.settings = CloudSettings{}, .across = glm::dvec2(0.0), .heightAbove = 1500.0f,
                                   .present = true};
    CHECK(SkyProbeInputsDiffer(clear, cloudy, 0.5f));
    CHECK(SkyProbeInputsDiffer(cloudy, clear, 0.5f));
    CHECK_FALSE(SkyProbeInputsDiffer(cloudy, cloudy, 0.5f));

    SkyProbeInputs drifted = cloudy;
    drifted.clouds.across = glm::dvec2(200.0, 0.0);
    CHECK(SkyProbeInputsDiffer(cloudy, drifted, 0.5f));
    drifted.clouds.across = glm::dvec2(1.0, 0.0);
    CHECK_FALSE(SkyProbeInputsDiffer(cloudy, drifted, 0.5f));

    SkyProbeInputs thicker = cloudy;
    thicker.clouds.settings.coverage = 0.9f;
    CHECK(SkyProbeInputsDiffer(cloudy, thicker, 0.5f));
}

TEST_CASE("cloud quality is brought into range, and its tiers are recognised")
{
    const CloudQuality wild = Sanitized(CloudQuality{.resolution = 3000, .steps = 1});
    CHECK(wild.resolution == 2048u);
    CHECK(wild.steps == kMinCloudSteps);
    CHECK(Sanitized(CloudQuality{.resolution = 1u << 20u, .steps = 100000}).resolution == kMaxCloudMapResolution);

    for (CloudTier tier : {CloudTier::Low, CloudTier::Medium, CloudTier::High, CloudTier::Ultra})
    {
        CHECK(Tier(CloudTierSettings(tier)) == tier);
    }
    CHECK(Tier(CloudQuality{.resolution = 512, .steps = 200}) == CloudTier::Custom);
}
