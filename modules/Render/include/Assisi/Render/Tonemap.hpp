/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Tonemap.hpp
/// @brief The look the post chain ends on: exposure, a tone curve, and a grade.
///
/// The curves themselves are in tonemap.frag. What lives here is everything the
/// CPU has to agree with that shader about — the operator's wire value, the push
/// constants, the ranges a value is allowed to take, and the rule that a
/// copy-through step neither exposes nor grades.

#include <Assisi/Math/Float.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Assisi::Render
{

/// @brief Which tone curve maps radiance to display values.
///
/// These values are the wire encoding — tonemap.frag switches on the integer, so
/// reordering them changes the look of every saved options.json rather than
/// failing to build.
enum class TonemapOperator : std::uint8_t
{
    /// Inset matrix, log2 sigmoid, outset matrix. Bright saturated colour
    /// desaturates toward white instead of rotating hue, and gradation survives
    /// much further into the highlights than a per-channel curve leaves it.
    AgX = 0,
    /// The Narkowicz RRT/ODT fit. Contrastier untouched, at the cost of hue: a
    /// saturated blue climbs toward purple, a red toward orange, and everything
    /// bright converges on a corner of the RGB cube. Reaches display white at
    /// about 7.2x linear, above which highlights carry nothing.
    Aces = 1,
    /// x/(1+x). What the chain applied before it had a choice, kept because a
    /// flat reference is what makes the other two legible.
    Reinhard = 2,
};

inline constexpr std::uint32_t kTonemapOperatorCount = 3;

/// @brief Stops of exposure. Beyond this range every scene is either black or
/// white, so the slider stops being a comparison tool.
inline constexpr float kMinExposureStops = -8.0f;
inline constexpr float kMaxExposureStops = 8.0f;

/// @brief Exponent on the curve's output. Below 1 flattens toward grey; above
/// 2.5 crushes everything but the highlights to black.
inline constexpr float kMinContrast = 0.5f;
inline constexpr float kMaxContrast = 2.5f;

/// @brief Scale on each channel's distance from the pixel's luma. 0 is
/// greyscale; past 2 the primaries clip and hue stops meaning anything.
inline constexpr float kMinSaturation = 0.0f;
inline constexpr float kMaxSaturation = 2.0f;

/// @brief The grade AgX is designed to be finished with — flat is what its
/// neutrality costs, and this is what buys the contrast back. Applied whichever
/// operator is selected, so switching to ACES stacks it on a curve that already
/// has an opinion.
inline constexpr float kPunchyContrast = 1.35f;
inline constexpr float kPunchySaturation = 1.4f;

/// @brief The look, as the user edits it.
struct TonemapSettings
{
    TonemapOperator op = TonemapOperator::AgX;
    float exposureStops = 0.0f;
    float contrast = kPunchyContrast;
    float saturation = kPunchySaturation;
    /// Noise of up to one display step before the 8-bit write, which turns the
    /// bands a slow fade steps through into an even grain. See DisplayDither.
    bool dither = true;
};

/// @brief Clamps to [low, high], substituting `fallback` for a non-finite value.
///
/// std::clamp alone returns NaN unchanged — both of its comparisons are false —
/// and a NaN in any of these lanes takes the whole frame to black.
[[nodiscard]] inline float ClampFinite(float value, float low, float high, float fallback)
{
    return Math::IsFinite(value) ? std::clamp(value, low, high) : fallback;
}

/// @brief The same settings with every lane inside its range. options.json is
/// hand-editable, so nothing downstream may assume the values are sane.
[[nodiscard]] inline TonemapSettings Sanitized(TonemapSettings settings)
{
    const TonemapSettings defaults;
    if (static_cast<std::uint32_t>(settings.op) >= kTonemapOperatorCount)
    {
        settings.op = defaults.op;
    }
    settings.exposureStops = ClampFinite(settings.exposureStops, kMinExposureStops, kMaxExposureStops, 0.0f);
    settings.contrast = ClampFinite(settings.contrast, kMinContrast, kMaxContrast, defaults.contrast);
    settings.saturation = ClampFinite(settings.saturation, kMinSaturation, kMaxSaturation, defaults.saturation);
    return settings;
}

/// @brief The linear multiplier exposure applies to radiance before the curve.
[[nodiscard]] inline float ExposureScale(float stops)
{
    return std::exp2(ClampFinite(stops, kMinExposureStops, kMaxExposureStops, 0.0f));
}

/// @brief The tone map's push constants. Field order and types match
/// tonemap.frag's PushConstants block.
struct TonemapConstants
{
    float exposureScale = 1.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    std::uint32_t op = static_cast<std::uint32_t>(TonemapOperator::AgX);
    std::uint32_t passthrough = 0;
    /// One display step in output units while dithering, else 0.
    float ditherScale = 0.0f;
};

/// @brief The steps an 8-bit display target has above black.
inline constexpr float kDisplaySteps = 255.0f;

/// @brief Scrambles @p value's bits so neighbouring inputs land far apart. The
/// lowbias32 integer hash; its constants are the function. tonemap.frag carries
/// the same one.
[[nodiscard]] constexpr std::uint32_t DitherHash(std::uint32_t value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

/// @brief How far, in display steps, the tone map moves pixel (@p x, @p y)
/// before the 8-bit write: in (-1, 1), most often near 0.
///
/// The image is rendered in float and the display keeps 256 values per channel,
/// so a slow fade holds one value and then jumps to the next, a band. Noise of
/// up to a step either way spreads each pixel over the two steps it falls
/// between, in proportion to how near it is to each, so a patch of them looks
/// like the value between. Triangular, the sum of two uniform draws, because
/// that leaves the noise's strength the same whatever the value; uniform noise
/// is visibly stronger halfway between two steps. The same pattern every frame:
/// nothing here accumulates over time. tonemap.frag computes the same values.
[[nodiscard]] constexpr float DisplayDither(std::uint32_t x, std::uint32_t y)
{
    // Two 16-bit uniform draws from one hash of the pixel, each taken at the
    // middle of its step so neither reaches 0 or 1 and the sum stays inside
    // (-1, 1).
    constexpr std::uint32_t kHalfBits = 16u;
    constexpr std::uint32_t kHalfMask = 0xffffu;
    constexpr float kHalfRange = 65536.0f;
    constexpr float kMidStep = 0.5f;
    const std::uint32_t hash = DitherHash(x ^ DitherHash(y));
    const float first = (static_cast<float>(hash & kHalfMask) + kMidStep) / kHalfRange;
    const float second = (static_cast<float>(hash >> kHalfBits) + kMidStep) / kHalfRange;
    return first + second - 1.0f;
}

/// @brief The constants for one tone map draw.
///
/// `copyThrough` covers the two steps that must move an image without restating
/// it: the Blit, whose input this same shader already mapped, and the material
/// debug views, whose scene target holds channel values rather than radiance.
/// Both come out as an identity copy — exposing, grading or dithering either
/// would apply the look a second time.
[[nodiscard]] inline TonemapConstants MakeTonemapConstants(const TonemapSettings &settings, bool copyThrough)
{
    if (copyThrough)
    {
        return TonemapConstants{.passthrough = 1};
    }

    const TonemapSettings safe = Sanitized(settings);
    return TonemapConstants{.exposureScale = ExposureScale(safe.exposureStops),
                            .contrast = safe.contrast,
                            .saturation = safe.saturation,
                            .op = static_cast<std::uint32_t>(safe.op),
                            .passthrough = 0,
                            .ditherScale = safe.dither ? 1.0f / kDisplaySteps : 0.0f};
}

} // namespace Assisi::Render
