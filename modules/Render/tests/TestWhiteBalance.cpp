/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/WhiteBalance.hpp>

#include <limits>

using namespace Assisi::Render;

namespace
{

void CheckNear(const glm::vec3 &actual, const glm::vec3 &expected, double epsilon)
{
    CHECK(actual.x == doctest::Approx(expected.x).epsilon(epsilon));
    CHECK(actual.y == doctest::Approx(expected.y).epsilon(epsilon));
    CHECK(actual.z == doctest::Approx(expected.z).epsilon(epsilon));
}

} // namespace

TEST_CASE("The neutral white balance changes nothing")
{
    const glm::mat3 neutral = WhiteBalanceMatrix(kNeutralWhiteBalanceKelvin, 0.0f);
    for (glm::length_t column = 0; column < 3; ++column)
    {
        for (glm::length_t row = 0; row < 3; ++row)
        {
            CAPTURE(column);
            CAPTURE(row);
            CHECK(neutral[column][row] == doctest::Approx(column == row ? 1.0f : 0.0f).epsilon(1e-4));
        }
    }
}

TEST_CASE("Light of the balanced colour comes out as the neutral white, at its own brightness")
{
    const glm::vec3 neutral = WhiteBalanceWhite(kNeutralWhiteBalanceKelvin, 0.0f);
    for (const float kelvin : {1900.0f, 3200.0f, 5000.0f, 9000.0f, 15000.0f})
    {
        for (const float tint : {-0.6f, 0.0f, 0.6f})
        {
            CAPTURE(kelvin);
            CAPTURE(tint);
            CheckNear(WhiteBalanceMatrix(kelvin, tint) * WhiteBalanceWhite(kelvin, tint), neutral, 1e-3);
        }
    }
}

TEST_CASE("Balanced below the light the picture cools, above it the picture warms")
{
    const glm::vec3 grey(0.5f);
    const glm::vec3 tungsten = WhiteBalanceMatrix(3000.0f, 0.0f) * grey;
    CHECK(tungsten.z > tungsten.x);
    const glm::vec3 shade = WhiteBalanceMatrix(10000.0f, 0.0f) * grey;
    CHECK(shade.x > shade.z);
}

TEST_CASE("A positive tint turns the picture magenta, a negative one green")
{
    const glm::vec3 grey(0.5f);
    const glm::vec3 magenta = WhiteBalanceMatrix(kNeutralWhiteBalanceKelvin, 0.8f) * grey;
    CHECK(magenta.y < 0.5f * (magenta.x + magenta.z));
    const glm::vec3 green = WhiteBalanceMatrix(kNeutralWhiteBalanceKelvin, -0.8f) * grey;
    CHECK(green.y > 0.5f * (green.x + green.z));
}

TEST_CASE("The white follows the black body line, warm to cool")
{
    // Candlelight is far redder than noon, and a clear sky bluer.
    CHECK(WhiteBalanceChromaticity(2000.0f, 0.0f).x > WhiteBalanceChromaticity(6500.0f, 0.0f).x);
    CHECK(WhiteBalanceChromaticity(12000.0f, 0.0f).x < WhiteBalanceChromaticity(6500.0f, 0.0f).x);
    const glm::vec2 noon = WhiteBalanceChromaticity(6500.0f, 0.0f);
    CHECK(noon.x == doctest::Approx(0.3135f).epsilon(0.005));
    CHECK(noon.y == doctest::Approx(0.3237f).epsilon(0.005));
}

TEST_CASE("A temperature or tint out of range is held at its end, and junk at neutral")
{
    CheckNear(WhiteBalanceWhite(100.0f, 0.0f), WhiteBalanceWhite(kMinWhiteBalanceKelvin, 0.0f), 1e-6);
    CheckNear(WhiteBalanceWhite(1.0e6f, 5.0f), WhiteBalanceWhite(kMaxWhiteBalanceKelvin, kMaxWhiteBalanceTint), 1e-6);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CheckNear(WhiteBalanceWhite(nan, nan), WhiteBalanceWhite(kNeutralWhiteBalanceKelvin, 0.0f), 1e-6);
}
