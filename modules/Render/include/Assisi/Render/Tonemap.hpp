/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Tonemap.hpp
/// @brief The look the post chain ends on: exposure, a tone curve, and a grade.
///
/// The curves themselves are in tonemap.frag. What lives here is everything the
/// CPU has to agree with that shader about — the operator's wire value, the push
/// constants, the ranges a value is allowed to take, and the rule that a
/// copy-through step neither exposes nor grades.

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
};

/// @brief Clamps to [low, high], substituting `fallback` for a non-finite value.
///
/// std::clamp alone returns NaN unchanged — both of its comparisons are false —
/// and a NaN in any of these lanes takes the whole frame to black.
[[nodiscard]] inline float ClampFinite(float value, float low, float high, float fallback)
{
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
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
};

/// @brief The constants for one tone map draw.
///
/// `copyThrough` covers the two steps that must move an image without restating
/// it: the Blit, whose input this same shader already mapped, and the material
/// debug views, whose scene target holds channel values rather than radiance.
/// Both come out as an identity copy — exposing or grading either would apply
/// the look a second time.
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
                            .passthrough = 0};
}

} // namespace Assisi::Render
