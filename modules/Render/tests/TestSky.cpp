/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Sky.hpp>

#include <cmath>
#include <limits>
#include <vector>

using namespace Assisi::Render;

namespace
{
/// A direction from an elevation above the horizon and an azimuth about +Y.
/// Elevation 90 is straight up; azimuth 0 points along +Z.
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

/// Directions spread over the whole sphere, ground included.
std::vector<glm::vec3> SphereLattice()
{
    std::vector<glm::vec3> directions;
    for (int32_t elevation = -90; elevation <= 90; elevation += 10)
    {
        for (int32_t azimuth = 0; azimuth < 360; azimuth += 15)
        {
            directions.push_back(Dir(static_cast<float>(elevation), static_cast<float>(azimuth)));
        }
    }
    return directions;
}

float Luminance(const glm::vec3 &linear)
{
    return glm::dot(linear, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

bool AllFinite(const glm::vec3 &v)
{
    return std::isfinite(v.r) && std::isfinite(v.g) && std::isfinite(v.b);
}

/// A sky with no haze and no disk, so a test about scattering is not reading the
/// aureole or the sun instead.
SkySettings ClearAir()
{
    SkySettings settings;
    settings.haze = 0.0f;
    settings.sunDiskIntensity = 0.0f;
    return settings;
}
} // namespace

TEST_CASE("Sky settings are sanitized into their ranges")
{
    SkySettings settings;
    settings.zenithOpticalDepth = std::numeric_limits<float>::quiet_NaN();
    settings.haze = 99.0f;
    settings.intensity = -4.0f;
    settings.sunAngularRadiusDegrees = 0.0f;
    settings.sunDiskIntensity = std::numeric_limits<float>::infinity();
    settings.groundColor = glm::vec3(-1.0f, std::numeric_limits<float>::quiet_NaN(), 0.5f);
    settings.nightColor = glm::vec3(std::numeric_limits<float>::infinity(), 0.25f, -0.25f);

    const SkySettings safe = Sanitized(settings);
    const SkySettings defaults;

    // NaN falls back to the default; a finite out-of-range value clamps.
    CHECK(safe.zenithOpticalDepth == doctest::Approx(defaults.zenithOpticalDepth));
    CHECK(safe.haze == doctest::Approx(kMaxHaze));
    CHECK(safe.intensity == doctest::Approx(kMinSkyIntensity));
    CHECK(safe.sunAngularRadiusDegrees == doctest::Approx(kMinSunAngularRadiusDegrees));
    CHECK(safe.sunDiskIntensity == doctest::Approx(defaults.sunDiskIntensity));
    CHECK(safe.groundColor.r == doctest::Approx(0.0f));
    CHECK(safe.groundColor.g == doctest::Approx(defaults.groundColor.g));
    CHECK(safe.groundColor.b == doctest::Approx(0.5f));
    CHECK(safe.nightColor.r == doctest::Approx(defaults.nightColor.r));
    CHECK(safe.nightColor.b == doctest::Approx(0.0f));

    // Idempotent: nothing downstream has to wonder whether it was already done.
    const SkySettings twice = Sanitized(safe);
    CHECK(twice.zenithOpticalDepth == doctest::Approx(safe.zenithOpticalDepth));
    CHECK(twice.haze == doctest::Approx(safe.haze));
    CHECK(twice.intensity == doctest::Approx(safe.intensity));
    CHECK(twice.sunDiskIntensity == doctest::Approx(safe.sunDiskIntensity));
}

TEST_CASE("A sun with no direction and no radiance is still a usable sun")
{
    SkySun sun;
    sun.directionToSun = glm::vec3(0.0f);
    sun.intensity = std::numeric_limits<float>::quiet_NaN();
    sun.color = glm::vec3(std::numeric_limits<float>::quiet_NaN(), -2.0f, 3.0f);

    const SkySun safe = Sanitized(sun);
    CHECK(glm::length(safe.directionToSun) == doctest::Approx(1.0f));
    CHECK(safe.intensity == doctest::Approx(1.0f));
    CHECK(safe.color.r == doctest::Approx(1.0f));
    CHECK(safe.color.g == doctest::Approx(0.0f));
    CHECK(safe.color.b == doctest::Approx(3.0f));

    // A sun of length 3 names the same direction as a sun of length 1.
    SkySun unnormalized = SunAt(30.0f, 40.0f);
    unnormalized.directionToSun *= 3.0f;
    const SkySun normalized = Sanitized(unnormalized);
    CHECK(normalized.directionToSun.x == doctest::Approx(Dir(30.0f, 40.0f).x));
    CHECK(normalized.directionToSun.y == doctest::Approx(Dir(30.0f, 40.0f).y));
}

TEST_CASE("Air mass grows with the zenith angle and keeps growing after sunset")
{
    // Looking up crosses one atmosphere by definition; the horizon crosses tens.
    CHECK(ViewAirMass(1.0f) == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(ViewAirMass(0.0f) > 30.0f);

    float previous = ViewAirMass(1.0f);
    for (int32_t elevation = 89; elevation >= 0; --elevation)
    {
        const float airMass = ViewAirMass(std::sin(glm::radians(static_cast<float>(elevation))));
        CHECK(airMass >= previous);
        previous = airMass;
    }

    // The sun's path keeps lengthening below the horizon — that continuation is
    // the whole of dusk, and holding it flat would leave a lit sky at midnight.
    previous = SunAirMass(0.0f);
    for (int32_t elevation = -1; elevation >= -90; --elevation)
    {
        const float airMass = SunAirMass(std::sin(glm::radians(static_cast<float>(elevation))));
        CHECK(airMass > previous);
        CHECK(std::isfinite(airMass));
        previous = airMass;
    }
}

TEST_CASE("Every direction and every sun position yields finite, non-negative radiance")
{
    const SkySettings settings;
    for (int32_t sunElevation = -90; sunElevation <= 90; sunElevation += 5)
    {
        const SkySun sun = SunAt(static_cast<float>(sunElevation));
        for (const glm::vec3 &direction : SphereLattice())
        {
            const glm::vec3 radiance = SkyRadiance(direction, sun, settings);
            CHECK(AllFinite(radiance));
            CHECK(radiance.r >= 0.0f);
            CHECK(radiance.g >= 0.0f);
            CHECK(radiance.b >= 0.0f);
        }
    }

    // A degenerate direction is a fallback, never a NaN.
    CHECK(AllFinite(SkyRadiance(glm::vec3(0.0f), SunAt(45.0f), settings)));
}

TEST_CASE("Radiance sanitizes what it is handed, so a bad config cannot black out the frame")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();

    SkySettings broken;
    broken.zenithOpticalDepth = nan;
    broken.haze = -5.0f;
    broken.intensity = std::numeric_limits<float>::infinity();
    broken.groundColor = glm::vec3(nan);
    broken.nightColor = glm::vec3(-1.0f);
    broken.sunAngularRadiusDegrees = nan;
    broken.sunDiskIntensity = nan;

    SkySun brokenSun;
    brokenSun.directionToSun = glm::vec3(0.0f);
    brokenSun.color = glm::vec3(nan, -1.0f, 2.0f);
    brokenSun.intensity = nan;

    const glm::vec3 direction = Dir(25.0f, 140.0f);
    const glm::vec3 radiance = SkyRadiance(direction, brokenSun, broken);
    CHECK(AllFinite(radiance));
    CHECK(radiance.r >= 0.0f);
    CHECK(radiance.g >= 0.0f);
    CHECK(radiance.b >= 0.0f);

    // And it is exactly what the sanitized inputs would have produced: the
    // clamping happens inside, so no caller has to remember to do it first.
    const glm::vec3 viaSanitized = SkyRadiance(direction, Sanitized(brokenSun), Sanitized(broken));
    CHECK(radiance.r == doctest::Approx(viaSanitized.r));
    CHECK(radiance.g == doctest::Approx(viaSanitized.g));
    CHECK(radiance.b == doctest::Approx(viaSanitized.b));
}

TEST_CASE("The daytime zenith is blue and the horizon is not")
{
    const SkySettings settings = ClearAir();
    const SkySun sun = SunAt(45.0f, 0.0f);

    // Both sampled a quarter turn away from the sun in azimuth, so the aureole
    // around it is not what either reading is measuring.
    const glm::vec3 zenith = SkyRadiance(Dir(89.0f, 90.0f), sun, settings);
    const glm::vec3 horizon = SkyRadiance(Dir(1.0f, 90.0f), sun, settings);

    // Blue dominates overhead, where the ray crosses little air. Toward the
    // horizon the blue has already scattered out and the colour washes toward
    // white, so the ratio falls.
    CHECK(zenith.b / zenith.r > 3.0f);
    CHECK(horizon.b / horizon.r < zenith.b / zenith.r);

    // And the horizon is brighter, having far more air to scatter in.
    CHECK(Luminance(horizon) > Luminance(zenith));
}

TEST_CASE("A low sun reddens, which is what carries the day cycle")
{
    const SkySettings settings = ClearAir();

    const glm::vec3 noon = SkyRadiance(Dir(90.0f, 0.0f), SunAt(90.0f), settings);
    const glm::vec3 sunset = SkyRadiance(Dir(1.0f, 0.0f), SunAt(1.0f), settings);

    // Looking straight at the sun in both cases: overhead it is blue-white,
    // and at the horizon its own path through the air has taken the blue out.
    CHECK(noon.r / noon.b < 1.0f);
    CHECK(sunset.r / sunset.b > 2.0f);

    // The reddening is monotone as the sun descends, not a step at some angle.
    float previousRatio = 0.0f;
    for (int32_t elevation = 60; elevation >= 0; elevation -= 5)
    {
        const glm::vec3 radiance = SkyRadiance(Dir(1.0f, 0.0f), SunAt(static_cast<float>(elevation)), settings);
        const float ratio = radiance.r / radiance.b;
        CHECK(ratio > previousRatio);
        previousRatio = ratio;
    }
}

TEST_CASE("Night is dark, and never darker than the night floor")
{
    const SkySettings settings;

    const auto skyEnergy = [&settings](float sunElevation)
                           {
                               const SkySun sun = SunAt(sunElevation);
                               float total = 0.0f;
                               for (const glm::vec3 &direction : SphereLattice())
                               {
                                   total += Luminance(SkyRadiance(direction, sun, settings));
                               }
                               return total;
                           };

    CHECK(skyEnergy(-30.0f) < skyEnergy(0.0f));
    CHECK(skyEnergy(0.0f) < skyEnergy(60.0f));

    // Deep night still reads as a sky rather than as a hole: the night colour is
    // added, so it is the floor every direction sits on.
    const glm::vec3 midnight = SkyRadiance(Dir(90.0f, 0.0f), SunAt(-60.0f), settings);
    CHECK(midnight.b >= settings.nightColor.b * settings.intensity * 0.99f);
    CHECK(Luminance(midnight) < 0.01f);

    // The ground falls to the same floor rather than going black under it while
    // the sky above still glows — which is what it would do if a sun below the
    // horizon were allowed to light it by a negative amount.
    const glm::vec3 groundAtDusk = SkyRadiance(glm::vec3(0.0f, -1.0f, 0.0f), SunAt(-1.0f), settings);
    CHECK(groundAtDusk.r >= settings.nightColor.r * settings.intensity * 0.99f);
    CHECK(groundAtDusk.b >= settings.nightColor.b * settings.intensity * 0.99f);
}

TEST_CASE("The horizon is a seam, not a step")
{
    const SkySettings settings;
    const SkySun sun = SunAt(30.0f, 0.0f);

    // Straddling the blend band: the sky and the ground meet inside it, so a
    // pair of directions either side of it must not differ by a visible jump.
    for (int32_t azimuth = 0; azimuth < 360; azimuth += 45)
    {
        const glm::vec3 above = SkyRadiance(Dir(1.0f, static_cast<float>(azimuth)), sun, settings);
        const glm::vec3 below = SkyRadiance(Dir(-1.0f, static_cast<float>(azimuth)), sun, settings);
        CHECK(Luminance(above) > 0.0f);
        CHECK(Luminance(below) > 0.0f);
    }

    // Exactly at the horizon the blend is half and half, so the seam lands
    // between the sky just above the band and the ground just below it rather
    // than jumping past either.
    const glm::vec3 seam = SkyRadiance(glm::vec3(0.0f, 0.0f, 1.0f), sun, settings);
    const glm::vec3 sky = SkyRadiance(Dir(1.0f, 0.0f), sun, settings);
    const glm::vec3 ground = SkyRadiance(Dir(-1.0f, 0.0f), sun, settings);
    CHECK(Luminance(seam) < Luminance(sky));
    CHECK(Luminance(seam) > Luminance(ground));
}

TEST_CASE("Looking down gives the ground, in the ground's own colour")
{
    SkySettings settings = ClearAir();
    settings.groundColor = glm::vec3(0.4f, 0.2f, 0.1f);
    settings.nightColor = glm::vec3(0.0f);
    const SkySun sun = SunAt(90.0f);

    const glm::vec3 down = SkyRadiance(glm::vec3(0.0f, -1.0f, 0.0f), sun, settings);

    // The ground is the sun's light off the ground albedo, so the channel ratios
    // are the albedo's, tinted only by what the beam lost on the way in.
    CHECK(down.r / down.g == doctest::Approx(2.0f).epsilon(0.08));
    CHECK(down.g / down.b == doctest::Approx(2.0f).epsilon(0.08));

    // And it darkens with the sun, because it is lit by the same beam.
    const glm::vec3 lowSun = SkyRadiance(glm::vec3(0.0f, -1.0f, 0.0f), SunAt(10.0f), settings);
    CHECK(Luminance(lowSun) < Luminance(down));

    // With the sun set there is no beam left, and the night colour is zero here.
    const glm::vec3 setSun = SkyRadiance(glm::vec3(0.0f, -1.0f, 0.0f), SunAt(-20.0f), settings);
    CHECK(Luminance(setSun) == doctest::Approx(0.0f));
}

TEST_CASE("The sun disk sits where the sun is, and turns off cleanly")
{
    SkySettings settings;
    settings.sunDiskIntensity = 50.0f;

    for (const float elevation : {10.0f, 45.0f, 80.0f})
    {
        for (const float azimuth : {0.0f, 120.0f, 250.0f})
        {
            const SkySun sun = SunAt(elevation, azimuth);
            const glm::vec3 atSun = SkyRadiance(sun.directionToSun, sun, settings);
            const glm::vec3 besideSun = SkyRadiance(Dir(elevation + 5.0f, azimuth), sun, settings);
            const glm::vec3 awayFromSun = SkyRadiance(Dir(elevation, azimuth + 180.0f), sun, settings);

            CHECK(Luminance(atSun) > Luminance(besideSun) * 5.0f);
            CHECK(Luminance(besideSun) > Luminance(awayFromSun));
        }
    }

    // Turning the disk off leaves the scattering that surrounds it untouched:
    // five degrees off the sun reads the same either way.
    const SkySun sun = SunAt(45.0f);
    SkySettings noDisk = settings;
    noDisk.sunDiskIntensity = 0.0f;
    CHECK(Luminance(SkyRadiance(sun.directionToSun, sun, noDisk)) < Luminance(SkyRadiance(sun.directionToSun, sun, settings)));
    CHECK(Luminance(SkyRadiance(Dir(50.0f, 0.0f), sun, noDisk)) ==
          doctest::Approx(Luminance(SkyRadiance(Dir(50.0f, 0.0f), sun, settings))));

    // A set sun takes its disk with it rather than shining up through the ground.
    // One degree either side of the horizon, where the beam is still strong
    // enough that the disk would be plain to see if nothing hid it: the whole
    // difference has to come from the ground being in the way.
    const SkySun justRisen = SunAt(1.0f);
    const SkySun justSet = SunAt(-1.0f);
    CHECK(Luminance(SkyRadiance(justRisen.directionToSun, justRisen, settings)) > 5.0f);
    CHECK(Luminance(SkyRadiance(justSet.directionToSun, justSet, settings)) < 1.0f);
}

TEST_CASE("Sky intensity scales the whole image and zero blanks it")
{
    SkySettings settings;
    const SkySun sun = SunAt(40.0f);
    const glm::vec3 direction = Dir(20.0f, 70.0f);

    settings.intensity = 1.0f;
    const glm::vec3 unit = SkyRadiance(direction, sun, settings);
    settings.intensity = 3.0f;
    const glm::vec3 tripled = SkyRadiance(direction, sun, settings);
    CHECK(tripled.r == doctest::Approx(unit.r * 3.0f));
    CHECK(tripled.b == doctest::Approx(unit.b * 3.0f));

    settings.intensity = 0.0f;
    const glm::vec3 blank = SkyRadiance(direction, sun, settings);
    CHECK(Luminance(blank) == doctest::Approx(0.0f));
}

TEST_CASE("Haze whitens the sky and grows the halo around the sun")
{
    SkySettings clear = ClearAir();
    SkySettings hazy = clear;
    hazy.haze = kMaxHaze;

    const SkySun sun = SunAt(30.0f, 0.0f);
    const glm::vec3 nearSun = Dir(38.0f, 0.0f);

    const glm::vec3 clearNearSun = SkyRadiance(nearSun, sun, clear);
    const glm::vec3 hazyNearSun = SkyRadiance(nearSun, sun, hazy);

    // Haze scatters every wavelength alike, so it adds a grey lobe: brighter
    // beside the sun, and less blue than the same sky without it.
    CHECK(Luminance(hazyNearSun) > Luminance(clearNearSun));
    CHECK(hazyNearSun.b / hazyNearSun.r < clearNearSun.b / clearNearSun.r);
}

TEST_CASE("Sky constants carry the sanitized settings and a unit sun")
{
    SkySettings settings;
    settings.haze = 99.0f;                                     // clamps
    settings.intensity = std::numeric_limits<float>::quiet_NaN(); // falls back
    settings.sunAngularRadiusDegrees = 2.0f;
    settings.sunDiskIntensity = 7.0f;
    settings.groundColor = glm::vec3(0.3f, 0.2f, 0.1f);

    SkySun sun = SunAt(35.0f, 15.0f);
    sun.directionToSun *= 5.0f;
    sun.color = glm::vec3(1.0f, 0.9f, 0.8f);
    sun.intensity = 2.0f;

    const glm::mat4 invViewProjection = glm::inverse(glm::perspective(glm::radians(60.0f), 1.6f, 0.1f, 100.0f));
    const SkyConstants constants = MakeSkyConstants(invViewProjection, glm::vec3(1.0f, 2.0f, 3.0f), sun, settings);

    CHECK(constants.cameraPosition.x == doctest::Approx(1.0f));
    CHECK(constants.cameraPosition.z == doctest::Approx(3.0f));
    CHECK(glm::length(glm::vec3(constants.sunDirection)) == doctest::Approx(1.0f));

    // Radiance is the colour already multiplied by intensity, so the shader
    // multiplies nothing the CPU could have.
    CHECK(constants.sunRadiance.r == doctest::Approx(2.0f));
    CHECK(constants.sunRadiance.g == doctest::Approx(1.8f));

    CHECK(constants.nightColor.w == doctest::Approx(kMaxHaze));
    CHECK(constants.params.x == doctest::Approx(SkySettings{}.intensity));
    CHECK(constants.params.y == doctest::Approx(7.0f));
    CHECK(constants.groundColor.w == doctest::Approx(settings.zenithOpticalDepth));

    // The two disk edges bracket the radius, outer first — the smoothstep in
    // sky.frag reads them in that order.
    CHECK(constants.sunDirection.w < constants.sunRadiance.w);
    CHECK(constants.sunRadiance.w < 1.0f);
}
