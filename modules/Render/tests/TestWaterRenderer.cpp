/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Render/Water.hpp>
#include <Assisi/Render/WaterFrame.hpp>
#include <Assisi/Render/WaterRenderer.hpp>

#include <cmath>

using namespace Assisi::Render;

namespace
{

WaterBody Sea()
{
    WaterBody body;
    body.region.origin = glm::dvec3(0.0, -1.5, 0.0);
    body.waves = WaterWaves{.wind = WindSea{.windSpeed = 9.0f, .bearingDegrees = 45.0f, .spread = 0.3f},
                            .swell = Swell{.height = 1.2f, .period = 12.0f, .bearingDegrees = 200.0f},
                            .choppiness = 0.8f,
                            .seed = 42u};
    body.cascades = LayoutCascades(body.waves);
    body.significantHeight = 1.7f;
    body.look = WaterLook{.color = glm::vec3(0.01f, 0.02f, 0.03f), .crestColor = glm::vec3(0.1f, 0.2f, 0.2f), .clarity = 9.0f};
    return body;
}

} // namespace

TEST_CASE("The spectrum's constants carry each cascade's band and both spectra")
{
    const WaterBody body = Sea();
    const WaterSpectrumConstants constants = MakeWaterSpectrumConstants(body);
    for (uint32_t c = 0; c < kWaterCascadeCount; ++c)
    {
        CAPTURE(c);
        CHECK(constants.tileLength[static_cast<glm::length_t>(c)] == body.cascades.tileLength[c]);
        CHECK(constants.minWavenumber[static_cast<glm::length_t>(c)] == body.cascades.minWavenumber[c]);
        CHECK(constants.maxWavenumber[static_cast<glm::length_t>(c)] == body.cascades.maxWavenumber[c]);
    }
    const WaveSpectrum wind = SpectrumOf(body.waves.wind);
    const WaveSpectrum swell = SpectrumOf(body.waves.swell);
    CHECK(constants.wind.x == wind.peakFrequency);
    CHECK(constants.wind.y == wind.alpha);
    CHECK(constants.wind.w == doctest::Approx(glm::radians(45.0f)));
    CHECK(constants.swell.x == swell.peakFrequency);
    CHECK(constants.spreads.x == doctest::Approx(0.3f));
    CHECK(constants.spreads.w == doctest::Approx(1.0f));
    CHECK(constants.seed.x == 42u);
}

TEST_CASE("The surface's constants place the waves and the body as the camera sees them")
{
    const WaterBody sea = Sea();
    const glm::dvec3 camera(123456.25, 4.0, -98765.5);
    const WaterDrawConstants constants = MakeWaterDrawConstants(sea, camera);

    // Each cascade's offset is the camera's place in its tile.
    for (uint32_t c = 0; c < kWaterCascadeCount; ++c)
    {
        CAPTURE(c);
        const glm::vec2 expected = CascadeOffset(glm::dvec2(camera.x, camera.z), static_cast<double>(sea.cascades.tileLength[c]));
        const glm::vec4 pair = constants.cascadeOffsets[c / 2u];
        const glm::vec2 offset = (c & 1u) == 0u ? glm::vec2(pair.x, pair.y) : glm::vec2(pair.z, pair.w);
        CHECK(offset.x == doctest::Approx(expected.x));
        CHECK(offset.y == doctest::Approx(expected.y));
    }
    // The still water is 5.5 m below the camera.
    CHECK(constants.surface.x == doctest::Approx(-5.5f));
    CHECK(constants.surface.y == doctest::Approx(1.7f));
    CHECK(constants.color.w == doctest::Approx(9.0f));
    // An ocean reaches past anything drawn.
    CHECK(constants.bounds.x < -1.0e8f);
    CHECK(constants.bounds.w > 1.0e8f);

    // A lake's rectangle is measured from the camera in its own frame.
    WaterBody lake = Sea();
    lake.region = WaterRegion{.origin = glm::dvec3(100.0, 20.0, -50.0), .axisX = glm::vec2(1.0f, 0.0f),
                              .halfExtent = glm::vec2(15.0f, 10.0f)};
    const WaterDrawConstants lakeConstants = MakeWaterDrawConstants(lake, glm::dvec3(110.0, 25.0, -45.0));
    CHECK(lakeConstants.bounds.x == doctest::Approx(-25.0f));
    CHECK(lakeConstants.bounds.y == doctest::Approx(-15.0f));
    CHECK(lakeConstants.bounds.z == doctest::Approx(5.0f));
    CHECK(lakeConstants.bounds.w == doctest::Approx(5.0f));
    CHECK(lakeConstants.surface.x == doctest::Approx(-5.0f));
}
