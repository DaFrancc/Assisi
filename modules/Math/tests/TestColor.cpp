/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Math/Color.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <type_traits>

using Assisi::Math::Color3;
using Assisi::Math::Color4;
using Assisi::Math::ColorSpace;

// The two spaces do not convert into each other, in either direction, even
// though both are a vector underneath; a plain vector still converts into either.
static_assert(!std::is_constructible_v<Color4<ColorSpace::Linear>, Color4<ColorSpace::Srgb>>);
static_assert(!std::is_constructible_v<Color4<ColorSpace::Srgb>, Color4<ColorSpace::Linear>>);
static_assert(!std::is_convertible_v<Color4<ColorSpace::Srgb>, Color4<ColorSpace::Linear>>);
static_assert(!std::is_constructible_v<Color3<ColorSpace::Linear>, Color3<ColorSpace::Srgb>>);
static_assert(!std::is_constructible_v<Color3<ColorSpace::Srgb>, Color3<ColorSpace::Linear>>);
static_assert(std::is_convertible_v<glm::vec4, Color4<ColorSpace::Srgb>>);
static_assert(std::is_convertible_v<Color4<ColorSpace::Linear>, Color4<ColorSpace::Linear>>);

TEST_CASE("Color: sRGB middle grey is about a fifth of the light, and converts back")
{
    // sRGB 0.5 is 0.21404 linear, the value every reference table gives.
    constexpr float kMiddleGreyLinear = 0.21404f;
    const Color3<ColorSpace::Srgb> grey{0.5f, 0.5f, 0.5f};

    const Color3<ColorSpace::Linear> linear = Assisi::Math::ToLinear(grey);
    CHECK(linear.r == doctest::Approx(kMiddleGreyLinear).epsilon(1e-4));
    CHECK(linear.g == doctest::Approx(kMiddleGreyLinear).epsilon(1e-4));
    CHECK(linear.b == doctest::Approx(kMiddleGreyLinear).epsilon(1e-4));

    const Color3<ColorSpace::Srgb> back = Assisi::Math::ToSrgb(linear);
    CHECK(back.r == doctest::Approx(0.5f));
}

TEST_CASE("Color: black and white are the same in both spaces, and the dark end is straight")
{
    const Color3<ColorSpace::Linear> ends = Assisi::Math::ToLinear(Color3<ColorSpace::Srgb>{0.f, 1.f, 0.02f});
    CHECK(ends.r == 0.f);
    CHECK(ends.g == doctest::Approx(1.f));
    CHECK(ends.b == doctest::Approx(0.02f / Assisi::Math::kSrgbSlope));
}

TEST_CASE("Color: converting a Color4 leaves alpha alone")
{
    const Color4<ColorSpace::Srgb> translucent{0.5f, 0.25f, 0.75f, 0.3f};
    CHECK(Assisi::Math::ToLinear(translucent).a == 0.3f);
    CHECK(Assisi::Math::ToSrgb(Color4<ColorSpace::Linear>{0.2f, 0.2f, 0.2f, 0.6f}).a == 0.6f);
}

TEST_CASE("Color: every channel survives a round trip through the other space")
{
    constexpr int32_t kSteps = 64;
    for (int32_t i = 0; i <= kSteps; ++i)
    {
        const float value = static_cast<float>(i) / static_cast<float>(kSteps);
        CAPTURE(value);
        CHECK(Assisi::Math::LinearToSrgb(Assisi::Math::SrgbToLinear(value)) == doctest::Approx(value).epsilon(1e-5));
    }
}
