/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Color.hpp
/// @brief Colour types — a vector in memory, a colour in a named space to the
/// compiler and the editor.
///
/// Color3/Color4 are layout- and codec-identical to glm::vec3/glm::vec4: the
/// same floats in the same order, and reflection serializes them through the
/// same JSON array, so a file written before a field changed type still loads
/// and a re-save is byte-identical. What the distinct type buys is at the point
/// of use — the reflected editor offers a colour picker for a Color and three
/// drag boxes for a direction, decided by the field's type rather than by a hint
/// that could be attached to any vector by mistake.
///
/// The space is part of the type, so the same floats in two spaces are two
/// types the compiler keeps apart: passing an sRGB colour where a linear one is
/// expected does not build, and crossing is an explicit ToLinear or ToSrgb.
/// Lighting and materials are linear, where numbers are proportional to light;
/// the UI is sRGB, where numbers are what a colour picker shows.
///
/// Derived from the glm type rather than wrapping one so that `.x`, the swizzles,
/// and every arithmetic operator keep working, and a Color passes anywhere its
/// vector is expected. glm's operators return plain vectors, which convert into
/// either space, so arithmetic is where the compiler stops checking. Only one
/// class in the hierarchy declares data members, so the types stay
/// standard-layout and offsetof stays valid — which reflection depends on.

#include <Assisi/Math/GLM.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Assisi::Math
{

/// @brief What a colour's numbers measure.
enum class ColorSpace : uint8_t
{
    Linear, ///< proportional to light; what lighting and blending maths needs
    Srgb,   ///< proportional to perceived brightness; what pickers and hex codes use
    Count
};

/// @brief RGB in @p Space.
template <ColorSpace Space>
struct Color3 : glm::vec3
{
    using glm::vec3::vec3;
    constexpr Color3(const glm::vec3 &v) : glm::vec3(v) {}

    // Without this a colour in the other space would convert through its base
    // vector into the constructor above.
    template <ColorSpace Other>
        requires(Other != Space)
    Color3(const Color3<Other> &) = delete;
};

/// @brief RGB with alpha, in @p Space. Alpha is coverage in either space, and is
/// never premultiplied.
template <ColorSpace Space>
struct Color4 : glm::vec4
{
    using glm::vec4::vec4;
    constexpr Color4(const glm::vec4 &v) : glm::vec4(v) {}

    template <ColorSpace Other>
        requires(Other != Space)
    Color4(const Color4<Other> &) = delete;
};

/// @name The sRGB transfer function
/// @{
/// At or below these a channel is on the curve's straight segment.
inline constexpr float kSrgbEncodedThreshold = 0.04045f;
inline constexpr float kSrgbLinearThreshold  = 0.0031308f;
/// The straight segment's slope, and the curved segment's offset, scale and power.
inline constexpr float kSrgbSlope  = 12.92f;
inline constexpr float kSrgbOffset = 0.055f;
inline constexpr float kSrgbScale  = 1.055f;
inline constexpr float kSrgbGamma  = 2.4f;
/// @}

/// @brief One sRGB channel as linear.
[[nodiscard]] inline float SrgbToLinear(float channel)
{
    return channel <= kSrgbEncodedThreshold ? channel / kSrgbSlope
                                            : std::pow((channel + kSrgbOffset) / kSrgbScale, kSrgbGamma);
}

/// @brief One linear channel as sRGB.
[[nodiscard]] inline float LinearToSrgb(float channel)
{
    return channel <= kSrgbLinearThreshold ? channel * kSrgbSlope
                                           : kSrgbScale * std::pow(channel, 1.f / kSrgbGamma) - kSrgbOffset;
}

[[nodiscard]] inline Color3<ColorSpace::Linear> ToLinear(const Color3<ColorSpace::Srgb> &color)
{
    return glm::vec3(SrgbToLinear(color.r), SrgbToLinear(color.g), SrgbToLinear(color.b));
}

[[nodiscard]] inline Color4<ColorSpace::Linear> ToLinear(const Color4<ColorSpace::Srgb> &color)
{
    return glm::vec4(SrgbToLinear(color.r), SrgbToLinear(color.g), SrgbToLinear(color.b), color.a);
}

[[nodiscard]] inline Color3<ColorSpace::Srgb> ToSrgb(const Color3<ColorSpace::Linear> &color)
{
    return glm::vec3(LinearToSrgb(color.r), LinearToSrgb(color.g), LinearToSrgb(color.b));
}

[[nodiscard]] inline Color4<ColorSpace::Srgb> ToSrgb(const Color4<ColorSpace::Linear> &color)
{
    return glm::vec4(LinearToSrgb(color.r), LinearToSrgb(color.g), LinearToSrgb(color.b), color.a);
}

/// @name Colour temperature
///
/// The range the Planckian fit below is defined over. Outside it the cubics
/// diverge rather than degrade, so the input is clamped rather than extrapolated.
/// @{
inline constexpr float kMinTemperatureKelvin = 1667.0f;
inline constexpr float kMaxTemperatureKelvin = 25000.0f;
/// @}

/// @brief The linear RGB of a blackbody at @p kelvin, normalised so its
/// strongest channel is one.
///
/// A COLOUR, not a brightness: how hot a source is says what hue it emits, and
/// how bright it is said separately by an intensity. Normalising is what keeps
/// those two independent, so retuning a light's temperature does not silently
/// change how much light it casts.
///
/// Warm is low — a candle is around 1900 K and a tungsten bulb 2800; daylight is
/// about 5800, an overcast sky 6500, and a blue north sky above 10000. That the
/// hot end is blue is a fact about physics and the opposite of how "warm" and
/// "cool" are used of the colours themselves.
///
/// The route is the Planckian locus in CIE xy (the Kim et al. cubic fit), then
/// xy to XYZ at unit luminance, then the sRGB primaries' matrix. Negative
/// channels are clamped away: a Planckian white outside the sRGB gamut has no
/// representation here, and the nearest in-gamut hue is a better answer than a
/// negative one.
[[nodiscard]] inline glm::vec3 BlackbodyColor(float kelvin)
{
    const float t = std::clamp(kelvin, kMinTemperatureKelvin, kMaxTemperatureKelvin);
    const float invT = 1.0f / t;
    const float invT2 = invT * invT;
    const float invT3 = invT2 * invT;

    const float x = t < 4000.0f
                        ? -0.2661239e9f * invT3 - 0.2343589e6f * invT2 + 0.8776956e3f * invT + 0.179910f
                        : -3.0258469e9f * invT3 + 2.1070379e6f * invT2 + 0.2226347e3f * invT + 0.240390f;
    const float x2 = x * x;
    const float x3 = x2 * x;

    float y;
    if (t < 2222.0f)
    {
        y = -1.1063814f * x3 - 1.34811020f * x2 + 2.18555832f * x - 0.20219683f;
    }
    else if (t < 4000.0f)
    {
        y = -0.9549476f * x3 - 1.37418593f * x2 + 2.09137015f * x - 0.16748867f;
    }
    else
    {
        y = 3.0817580f * x3 - 5.87338670f * x2 + 3.75112997f * x - 0.37001483f;
    }

    // Unit luminance, so what comes out differs only in hue.
    const float safeY = std::max(y, 1e-4f);
    const glm::vec3 xyz(x / safeY, 1.0f, (1.0f - x - y) / safeY);

    const glm::vec3 rgb(3.2404542f * xyz.x - 1.5371385f * xyz.y - 0.4985314f * xyz.z,
                        -0.9692660f * xyz.x + 1.8760108f * xyz.y + 0.0415560f * xyz.z,
                        0.0556434f * xyz.x - 0.2040259f * xyz.y + 1.0572252f * xyz.z);

    const glm::vec3 positive = glm::max(rgb, glm::vec3(0.0f));
    const float peak = std::max({positive.r, positive.g, positive.b});
    return peak > 0.0f ? positive / peak : glm::vec3(1.0f);
}

static_assert(sizeof(Color3<ColorSpace::Linear>) == sizeof(glm::vec3), "Color3 must stay layout-identical to its vector.");
static_assert(sizeof(Color3<ColorSpace::Srgb>) == sizeof(glm::vec3), "Color3 must stay layout-identical to its vector.");
static_assert(sizeof(Color4<ColorSpace::Linear>) == sizeof(glm::vec4), "Color4 must stay layout-identical to its vector.");
static_assert(sizeof(Color4<ColorSpace::Srgb>) == sizeof(glm::vec4), "Color4 must stay layout-identical to its vector.");

} /* namespace Assisi::Math */
