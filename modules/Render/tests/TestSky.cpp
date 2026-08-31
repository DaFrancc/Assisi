/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Math/Color.hpp>
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
    settings.hazeScattering = glm::vec3(0.0f);
    settings.sunDiskIntensity = 0.0f;
    return settings;
}
} // namespace

TEST_CASE("Sky settings are sanitized into their ranges")
{
    SkySettings settings;
    settings.airThickness = std::numeric_limits<float>::quiet_NaN();
    settings.hazeScattering = glm::vec3(-1.0f, 0.5f, 1e30f);
    settings.exposure = -4.0f;
    settings.sunSizeDegrees = 0.0f;
    settings.sunDiskIntensity = std::numeric_limits<float>::infinity();
    settings.groundColor = glm::vec3(-1.0f, std::numeric_limits<float>::quiet_NaN(), 0.5f);
    settings.nightColor = glm::vec3(std::numeric_limits<float>::infinity(), 0.25f, -0.25f);

    const SkySettings safe = Sanitized(settings);
    const SkySettings defaults;

    // NaN falls back to the default; a finite out-of-range value clamps.
    CHECK(safe.airThickness == doctest::Approx(defaults.airThickness));
    CHECK(safe.hazeScattering.r == doctest::Approx(0.0f));
    CHECK(safe.hazeScattering.g == doctest::Approx(0.5f));
    CHECK(safe.hazeScattering.b == doctest::Approx(kMaxSkyChannel));
    CHECK(safe.exposure == doctest::Approx(kMinSkyExposure));
    CHECK(safe.sunSizeDegrees == doctest::Approx(kMinSunSizeDegrees));
    CHECK(safe.sunDiskIntensity == doctest::Approx(defaults.sunDiskIntensity));
    CHECK(safe.groundColor.r == doctest::Approx(0.0f));
    CHECK(safe.groundColor.g == doctest::Approx(defaults.groundColor.g));
    CHECK(safe.groundColor.b == doctest::Approx(0.5f));
    CHECK(safe.nightColor.r == doctest::Approx(defaults.nightColor.r));
    CHECK(safe.nightColor.b == doctest::Approx(0.0f));

    // Idempotent: nothing downstream has to wonder whether it was already done.
    const SkySettings twice = Sanitized(safe);
    CHECK(twice.airThickness == doctest::Approx(safe.airThickness));
    CHECK(twice.hazeScattering.b == doctest::Approx(safe.hazeScattering.b));
    CHECK(twice.exposure == doctest::Approx(safe.exposure));
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
    broken.airThickness = nan;
    broken.hazeScattering = glm::vec3(-5.0f);
    broken.exposure = std::numeric_limits<float>::infinity();
    broken.groundColor = glm::vec3(nan);
    broken.nightColor = glm::vec3(-1.0f);
    broken.sunSizeDegrees = nan;
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
    CHECK(midnight.b >= settings.nightColor.b * settings.exposure * 0.99f);
    CHECK(Luminance(midnight) < 0.01f);

    // The ground falls to the same floor rather than going black under it while
    // the sky above still glows — which is what it would do if a sun below the
    // horizon were allowed to light it by a negative amount.
    const glm::vec3 groundAtDusk = SkyRadiance(glm::vec3(0.0f, -1.0f, 0.0f), SunAt(-1.0f), settings);
    CHECK(groundAtDusk.r >= settings.nightColor.r * settings.exposure * 0.99f);
    CHECK(groundAtDusk.b >= settings.nightColor.b * settings.exposure * 0.99f);
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

    settings.exposure = 1.0f;
    const glm::vec3 unit = SkyRadiance(direction, sun, settings);
    settings.exposure = 3.0f;
    const glm::vec3 tripled = SkyRadiance(direction, sun, settings);
    CHECK(tripled.r == doctest::Approx(unit.r * 3.0f));
    CHECK(tripled.b == doctest::Approx(unit.b * 3.0f));

    settings.exposure = 0.0f;
    const glm::vec3 blank = SkyRadiance(direction, sun, settings);
    CHECK(Luminance(blank) == doctest::Approx(0.0f));
}

TEST_CASE("Haze whitens the sky and grows the halo around the sun")
{
    SkySettings clear = ClearAir();
    SkySettings hazy = clear;
    hazy.hazeScattering = glm::vec3(0.1f);

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
    settings.hazeForwardness = 99.0f;                                // clamps
    settings.exposure = std::numeric_limits<float>::quiet_NaN(); // falls back
    settings.sunSizeDegrees = 2.0f;
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

    CHECK(constants.haze.w == doctest::Approx(kMaxHazeForwardness));
    CHECK(constants.groundColor.w == doctest::Approx(SkySettings{}.exposure));
    CHECK(constants.nightColor.w == doctest::Approx(7.0f));

    // The disk radius travels in radians, already converted, so the shader does
    // no trigonometry the CPU could have done once.
    CHECK(constants.sunDirection.w == doctest::Approx(glm::radians(2.0f)));

    // The Rayleigh lane carries the coefficients AS AUTHORED and the optical
    // depth beside them, NOT their product. The two are different quantities —
    // a ratio where they tint the in-scattered light, an extinction where they
    // attenuate it — and a lane holding the product cannot say which it is.
    // sky.frag once read a premultiplied lane as the ratio and rendered the sky
    // fourteen times too dark; this is what stops that being expressible.
    CHECK(constants.airScattering.r == doctest::Approx(settings.airScattering.r));
    CHECK(constants.airScattering.g == doctest::Approx(settings.airScattering.g));
    CHECK(constants.airScattering.b == doctest::Approx(settings.airScattering.b));
    CHECK(constants.airScattering.w == doctest::Approx(settings.airThickness));
    CHECK(constants.airScattering.b != doctest::Approx(settings.airScattering.b * settings.airThickness));
}

TEST_CASE("An airless world is the model's own limit, not a special case")
{
    SkySettings vacuum;
    vacuum.airThickness = 0.0f;
    vacuum.hazeScattering = glm::vec3(0.0f);
    vacuum.nightColor = glm::vec3(0.0f);
    vacuum.sunDiskIntensity = 0.0f;

    const SkySun sun = SunAt(45.0f);

    // No air to scatter in means no sky: every direction above the horizon is
    // black, at noon as much as at midnight.
    for (const float elevation : {5.0f, 45.0f, 89.0f})
    {
        const glm::vec3 radiance = SkyRadiance(Dir(elevation, 30.0f), sun, vacuum);
        CHECK(Luminance(radiance) == doctest::Approx(0.0f));
    }

    // And nothing dims the sun on its way in, so the ground gets the beam whole:
    // the lit ground is the albedo itself, untinted.
    SkySettings litGround = vacuum;
    litGround.groundColor = glm::vec3(0.5f, 0.4f, 0.3f);
    const glm::vec3 ground = SkyRadiance(glm::vec3(0.0f, -1.0f, 0.0f), SunAt(90.0f), litGround);
    CHECK(ground.r / ground.g == doctest::Approx(0.5f / 0.4f).epsilon(0.001));
    CHECK(ground.g / ground.b == doctest::Approx(0.4f / 0.3f).epsilon(0.001));

    // Air is what reddens a low sun. Without it the beam is the same colour at
    // the horizon as overhead — no atmosphere, no sunset.
    SkySettings withDisk = vacuum;
    withDisk.sunDiskIntensity = 10.0f;
    const glm::vec3 highSun = SkyRadiance(Dir(80.0f, 0.0f), SunAt(80.0f), withDisk);
    const glm::vec3 lowSun = SkyRadiance(Dir(1.0f, 0.0f), SunAt(1.0f), withDisk);
    CHECK(highSun.r / highSun.b == doctest::Approx(lowSun.r / lowSun.b).epsilon(0.001));
}

TEST_CASE("Air of another composition gives another sky, and its own sunset")
{
    SkySettings alien = ClearAir();
    // Green scatters hardest here, where on Earth blue does.
    alien.airScattering = glm::vec3(0.30f, 1.0f, 0.35f);

    const glm::vec3 noon = SkyRadiance(Dir(89.0f, 90.0f), SunAt(45.0f), alien);
    CHECK(noon.g > noon.r);
    CHECK(noon.g > noon.b);

    // And the sunset is the complement, with nothing further authored: the beam
    // loses what the air scatters most, so what survives a long path is what it
    // scatters least — red and blue, which is magenta.
    const glm::vec3 sunset = SkyRadiance(Dir(1.0f, 0.0f), SunAt(1.0f), alien);
    CHECK(sunset.r > sunset.g);
    CHECK(sunset.b > sunset.g);

    // The mechanism is the coefficients, not a hardcoded hue: Earth's air in the
    // same model puts blue overhead and red at the horizon instead.
    const SkySettings earth = ClearAir();
    const glm::vec3 earthNoon = SkyRadiance(Dir(89.0f, 90.0f), SunAt(45.0f), earth);
    CHECK(earthNoon.b > earthNoon.g);
}

TEST_CASE("Coloured haze tints the sky independently of the air")
{
    SkySettings dusty = ClearAir();
    dusty.airThickness = kMinAirThickness; // no molecular scattering at all
    dusty.hazeScattering = glm::vec3(0.06f, 0.03f, 0.01f);

    const SkySun sun = SunAt(30.0f, 0.0f);
    const glm::vec3 radiance = SkyRadiance(Dir(35.0f, 0.0f), sun, dusty);

    // With the air removed, the dust is the only thing scattering, and it is what
    // decides the colour — which is the case no amount of Rayleigh authoring reaches.
    CHECK(radiance.r > radiance.g);
    CHECK(radiance.g > radiance.b);
}

TEST_CASE("Mie asymmetry aims the halo, forward or back")
{
    SkySettings forward = ClearAir();
    // The air removed so the haze is the only thing scattering. Otherwise
    // Rayleigh — which peaks toward the sun whatever the haze is doing — buries
    // the effect this names, and the test would pass on the wrong grounds.
    forward.airThickness = kMinAirThickness;
    forward.hazeScattering = glm::vec3(0.05f);
    forward.hazeForwardness = 0.8f;

    SkySettings backward = forward;
    backward.hazeForwardness = -0.8f;

    // A low sun, so both samples sit above the horizon at the same elevation and
    // therefore in the same amount of air — the only thing separating them is
    // which way they face.
    const SkySun sun = SunAt(5.0f, 0.0f);
    const glm::vec3 toward = Dir(10.0f, 0.0f);
    const glm::vec3 away = Dir(10.0f, 180.0f);

    const glm::vec3 forwardNear = SkyRadiance(toward, sun, forward);
    const glm::vec3 forwardFar = SkyRadiance(away, sun, forward);
    const glm::vec3 backwardNear = SkyRadiance(toward, sun, backward);
    const glm::vec3 backwardFar = SkyRadiance(away, sun, backward);

    // Positive throws light on past the drop it scattered from, so the halo sits
    // around the sun; negative sends it back the way it came, and the bright half
    // of the sky is the half facing away.
    CHECK(Luminance(forwardNear) > Luminance(forwardFar));
    CHECK(Luminance(backwardFar) > Luminance(backwardNear));
}

TEST_CASE("The disk is a sphere, not a sticker")
{
    const float radius = glm::radians(2.0f);

    // Limb darkening: the rim carries less than the centre, by exactly the
    // parameter. Zero is the flat disk, which is what reads as fake.
    const float centre = SunDiskProfile(0.0f, radius, kMinSunEdgeSoftness, 0.6f);
    const float midway = SunDiskProfile(radius * 0.7f, radius, kMinSunEdgeSoftness, 0.6f);
    CHECK(centre == doctest::Approx(1.0f));
    CHECK(midway < centre);

    const float flatCentre = SunDiskProfile(0.0f, radius, kMinSunEdgeSoftness, 0.0f);
    const float flatMidway = SunDiskProfile(radius * 0.7f, radius, kMinSunEdgeSoftness, 0.0f);
    CHECK(flatMidway == doctest::Approx(flatCentre));

    // It falls off monotonically from the middle outward, and is gone outside.
    float previous = centre;
    for (int32_t step = 1; step <= 20; ++step)
    {
        const float profile = SunDiskProfile(radius * (static_cast<float>(step) / 20.0f), radius,
                                             kMinSunEdgeSoftness, 0.6f);
        CHECK(profile <= previous);
        previous = profile;
    }
    CHECK(SunDiskProfile(radius * 3.0f, radius, kDefaultSunEdgeSoftness, 0.6f) == doctest::Approx(0.0f));

    // A softer edge reaches further out — that is the whole of what it does, and
    // it is antialiasing rather than a glow.
    const float hard = SunDiskProfile(radius * 1.05f, radius, kMinSunEdgeSoftness, 0.0f);
    const float soft = SunDiskProfile(radius * 1.05f, radius, 0.5f, 0.0f);
    CHECK(soft > hard);

    // Finite everywhere, including the degenerate angles either side of it.
    CHECK(std::isfinite(SunDiskProfile(0.0f, radius, kMaxSunEdgeSoftness, kMaxSunLimbDarkening)));
    CHECK(std::isfinite(SunDiskProfile(glm::pi<float>(), radius, kMaxSunEdgeSoftness, kMaxSunLimbDarkening)));
}

TEST_CASE("The disk tint colours the sun without touching what lights the world")
{
    SkySettings settings;
    settings.sunDiskIntensity = 50.0f;
    SkySettings tinted = settings;
    tinted.sunDiskColor = glm::vec3(1.0f, 0.8f, 0.4f);

    const SkySun sun = SunAt(45.0f);

    // On the disk, the tint applies.
    const glm::vec3 plain = SkyRadiance(sun.directionToSun, sun, settings);
    const glm::vec3 yellow = SkyRadiance(sun.directionToSun, sun, tinted);
    CHECK(yellow.b < plain.b);
    CHECK(yellow.r / yellow.b > plain.r / plain.b);

    // Off it, nothing changed: the tint is the disk's alone, so the sky it sits
    // in — and the light the rest of the world is lit by — are untouched.
    const glm::vec3 offDiskPlain = SkyRadiance(Dir(20.0f, 90.0f), sun, settings);
    const glm::vec3 offDiskTinted = SkyRadiance(Dir(20.0f, 90.0f), sun, tinted);
    CHECK(offDiskTinted.r == doctest::Approx(offDiskPlain.r));
    CHECK(offDiskTinted.b == doctest::Approx(offDiskPlain.b));
}

TEST_CASE("The default sky is blue overhead and pale blue at the horizon")
{
    // What the defaults look like, asserted rather than left to a screenshot.
    // Every claim here is about the DEFAULT settings: change them and this is the
    // test that says the picture changed with them.
    const SkySettings settings;
    const SkySun sun = SunAt(45.0f, 0.0f);

    const glm::vec3 zenith = SkyRadiance(Dir(89.0f, 90.0f), sun, settings);
    const glm::vec3 horizon = SkyRadiance(Dir(1.0f, 90.0f), sun, settings);

    // Blue is the strongest channel in both halves of the sky. The horizon is
    // where this is easy to lose: single scattering alone puts GREEN highest
    // there, because it treats the blue knocked out over thirty air masses as
    // gone rather than re-scattered.
    CHECK(zenith.b > zenith.g);
    CHECK(zenith.g > zenith.r);
    CHECK(horizon.b > horizon.g);
    CHECK(horizon.g > horizon.r);

    // Deep overhead, pale at the horizon — the horizon is less saturated, not a
    // different hue.
    CHECK(zenith.b / zenith.r > 2.5f);
    CHECK(horizon.b / horizon.r < zenith.b / zenith.r);
    CHECK(horizon.b / horizon.r > 1.2f);

    // And brighter toward the horizon, which is what makes a sky read as deep.
    CHECK(Luminance(horizon) > Luminance(zenith));
}

TEST_CASE("Multiple scattering is what keeps the horizon from going green")
{
    SkySettings single;
    single.skyBounce = 0.0f;
    const SkySettings settings; // the default, which has it

    const SkySun sun = SunAt(45.0f, 0.0f);
    const glm::vec3 direction = Dir(1.0f, 90.0f);

    const glm::vec3 withoutIt = SkyRadiance(direction, sun, single);
    const glm::vec3 withIt = SkyRadiance(direction, sun, settings);

    // Without it the horizon's strongest channel is green — the artefact this
    // term exists to remove.
    CHECK(withoutIt.g > withoutIt.b);
    CHECK(withIt.b > withIt.g);

    // It adds light rather than redistributing it: the second bounce arrives, it
    // does not come out of the first.
    CHECK(Luminance(withIt) > Luminance(withoutIt));

    // And it costs nothing where there is no air to bounce in, so an airless
    // world is unaffected by whatever it is set to.
    SkySettings vacuum;
    vacuum.airThickness = 0.0f;
    vacuum.hazeScattering = glm::vec3(0.0f);
    vacuum.nightColor = glm::vec3(0.0f);
    vacuum.sunDiskIntensity = 0.0f;
    SkySettings vacuumMulti = vacuum;
    vacuumMulti.skyBounce = kMaxSkyBounce;
    CHECK(Luminance(SkyRadiance(direction, sun, vacuumMulti)) == doctest::Approx(0.0f));
}

TEST_CASE("The default sun disk is warm, not a white hole")
{
    const SkySettings settings;
    const SkySun sun = SunAt(50.0f);

    const glm::vec3 disk = SkyRadiance(sun.directionToSun, sun, settings);

    // Warm: red strongest, blue weakest, and by enough to read as yellow rather
    // than as a rounding difference.
    CHECK(disk.r > disk.g);
    CHECK(disk.g > disk.b);
    CHECK(disk.r / disk.b > 1.15f);

    // Still recognisably the sun rather than an orange lamp — a tint, not a
    // sunset.
    CHECK(disk.r / disk.b < 2.0f);
}

TEST_CASE("Sunlight at the ground is the same beam the sky scatters")
{
    const SkySettings settings;

    // Overhead, the air barely touches it: what lights the world is very nearly
    // the sun's own colour.
    const SkySun noon = SunAt(90.0f);
    const glm::vec3 noonLight = SunlightAtGround(noon, settings);
    CHECK(noonLight.r == doctest::Approx(1.0f).epsilon(0.05));
    CHECK(noonLight.r / noonLight.b == doctest::Approx(1.0f).epsilon(0.1));

    // Low, it is both dimmer and oranger, and both come off the one exponential
    // rather than from two knobs that could disagree.
    const SkySun sunset = SunAt(1.0f);
    const glm::vec3 sunsetLight = SunlightAtGround(sunset, settings);
    CHECK(Luminance(sunsetLight) < Luminance(noonLight));
    CHECK(sunsetLight.r / sunsetLight.b > 3.0f);

    // It tracks the sun continuously, with no step anywhere deciding when sunset
    // begins.
    float previous = 0.0f;
    for (int32_t elevation = 60; elevation >= 0; elevation -= 5)
    {
        const glm::vec3 light = SunlightAtGround(SunAt(static_cast<float>(elevation)), settings);
        const float ratio = light.r / light.b;
        CHECK(ratio > previous);
        previous = ratio;
    }

    // The sun's own colour still means what it says: the atmosphere works on it
    // rather than replacing it, so a blue sun stays a blue sun.
    SkySun blueSun = SunAt(70.0f);
    blueSun.color = glm::vec3(0.3f, 0.5f, 1.0f);
    const glm::vec3 blueLight = SunlightAtGround(blueSun, settings);
    CHECK(blueLight.b > blueLight.r);

    // And an airless world does nothing to it at all.
    SkySettings vacuum;
    vacuum.airThickness = 0.0f;
    const glm::vec3 unfiltered = SunlightAtGround(sunset, vacuum);
    CHECK(unfiltered.r == doctest::Approx(1.0f));
    CHECK(unfiltered.b == doctest::Approx(1.0f));
}

TEST_CASE("The transmittance the light takes is the one the sky applies")
{
    // The whole point of the toggle is that the two cannot drift: the light is
    // tinted by exactly the factor the sky already divides out of the beam. If
    // these ever disagree, a lit world stops matching the sky over it.
    const SkySettings settings;
    for (const float elevation : {80.0f, 40.0f, 5.0f, 0.5f})
    {
        const SkySun sun = SunAt(elevation);
        const glm::vec3 viaTransmittance =
            sun.color * sun.intensity * SunlightTransmittance(sun.directionToSun, settings);
        const glm::vec3 viaGround = SunlightAtGround(sun, settings);
        CHECK(viaTransmittance.r == doctest::Approx(viaGround.r));
        CHECK(viaTransmittance.g == doctest::Approx(viaGround.g));
        CHECK(viaTransmittance.b == doctest::Approx(viaGround.b));
    }

    // Never brightens: it is what SURVIVES the air, so it is a fraction.
    for (const float elevation : {90.0f, 30.0f, -10.0f})
    {
        const glm::vec3 t = SunlightTransmittance(Dir(elevation, 0.0f), settings);
        CHECK(t.r <= 1.0f);
        CHECK(t.b <= 1.0f);
        CHECK(t.b >= 0.0f);
        CHECK(AllFinite(t));
    }
}


TEST_CASE("A blackbody's colour is its hue, not its brightness")
{
    using Assisi::Math::BlackbodyColor;

    // Warm is low. That the hot end comes out blue is physics, and the opposite
    // of how "warm" and "cool" are used of the colours themselves.
    const glm::vec3 tungsten = BlackbodyColor(2800.0f);
    const glm::vec3 noonSun = BlackbodyColor(5778.0f);
    const glm::vec3 overcast = BlackbodyColor(6500.0f);
    const glm::vec3 northSky = BlackbodyColor(12000.0f);

    CHECK(tungsten.r > tungsten.b);
    CHECK(tungsten.r / tungsten.b > 4.0f);
    CHECK(noonSun.r > noonSun.b);
    CHECK(northSky.b > northSky.r);

    // 6500 K is the sRGB white point, so it comes out near enough neutral — the
    // check that the whole xy-to-RGB route is wired up the right way round.
    CHECK(overcast.r == doctest::Approx(1.0f).epsilon(0.06));
    CHECK(overcast.g == doctest::Approx(1.0f).epsilon(0.06));
    CHECK(overcast.b == doctest::Approx(1.0f).epsilon(0.06));

    // Monotone: every step up in temperature is a step toward blue, with no
    // temperature at which it doubles back.
    float previous = 1e9f;
    for (int32_t kelvin = 2000; kelvin <= 20000; kelvin += 250)
    {
        const glm::vec3 c = BlackbodyColor(static_cast<float>(kelvin));
        const float warmth = c.r / std::max(c.b, 1e-6f);
        CHECK(warmth < previous);
        previous = warmth;
    }

    // A hue and nothing more: the strongest channel is always one, so changing a
    // light's temperature never silently changes how much light it casts.
    for (const float kelvin : {1667.0f, 3000.0f, 5778.0f, 9000.0f, 25000.0f})
    {
        const glm::vec3 c = BlackbodyColor(kelvin);
        CHECK(std::max({c.r, c.g, c.b}) == doctest::Approx(1.0f));
        CHECK(c.r >= 0.0f);
        CHECK(c.g >= 0.0f);
        CHECK(c.b >= 0.0f);
        CHECK(AllFinite(c));
    }

    // Clamped rather than extrapolated: the cubic fit diverges outside its range,
    // so anything past the ends is the end.
    CHECK(BlackbodyColor(-500.0f).r == doctest::Approx(BlackbodyColor(Assisi::Math::kMinTemperatureKelvin).r));
    CHECK(BlackbodyColor(1e9f).b == doctest::Approx(BlackbodyColor(Assisi::Math::kMaxTemperatureKelvin).b));
    CHECK(AllFinite(BlackbodyColor(std::numeric_limits<float>::quiet_NaN())));
}
