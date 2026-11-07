/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PostProcessSettings.hpp
/// @brief How a scene's picture is finished: its exposure, grade, white
/// balance and glow, as the level's PostProcessVolumes set them for one frame.
///
/// The player's own choices, the tone curve, a brightness offset, dither and
/// whether bloom runs, are TonemapSettings and the options, not these.

#include <Assisi/Math/Float.hpp>
#include <Assisi/Render/Bloom.hpp>
#include <Assisi/Render/Exposure.hpp>
#include <Assisi/Render/WhiteBalance.hpp>

#include <algorithm>

namespace Assisi::Render
{

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

/// @brief Clamps to [low, high], substituting `fallback` for a non-finite value.
///
/// std::clamp alone returns NaN unchanged — both of its comparisons are false —
/// and a NaN in any of these lanes takes the whole frame to black.
[[nodiscard]] inline float ClampFinite(float value, float low, float high, float fallback)
{
    return Math::IsFinite(value) ? std::clamp(value, low, high) : fallback;
}

/// @brief A frame's finishing, before the player's choices.
///
/// Every default is what a scene with no PostProcessVolume is drawn with.
struct PostProcessSettings
{
    ExposureSettings exposure{};
    float contrast = kPunchyContrast;
    float saturation = kPunchySaturation;
    /// The colour of light that comes out white. See WhiteBalance.hpp.
    float whiteBalanceKelvin = kNeutralWhiteBalanceKelvin;
    float whiteBalanceTint = 0.0f;
    /// The share of the picture the bloom's glow replaces, and how widely it
    /// spreads. See Bloom.hpp.
    float bloomIntensity = kDefaultBloomIntensity;
    float bloomScatter = kDefaultBloomScatter;

    bool operator==(const PostProcessSettings &) const = default;
};

/// @brief The same settings with every lane in range. Idempotent.
[[nodiscard]] inline PostProcessSettings Sanitized(PostProcessSettings settings)
{
    const PostProcessSettings defaults;
    settings.exposure = Sanitized(settings.exposure);
    settings.contrast = ClampFinite(settings.contrast, kMinContrast, kMaxContrast, defaults.contrast);
    settings.saturation = ClampFinite(settings.saturation, kMinSaturation, kMaxSaturation, defaults.saturation);
    settings.whiteBalanceKelvin = ClampFinite(settings.whiteBalanceKelvin, kMinWhiteBalanceKelvin,
                                              kMaxWhiteBalanceKelvin, defaults.whiteBalanceKelvin);
    settings.whiteBalanceTint =
        ClampFinite(settings.whiteBalanceTint, kMinWhiteBalanceTint, kMaxWhiteBalanceTint, defaults.whiteBalanceTint);
    settings.bloomIntensity =
        ClampFinite(settings.bloomIntensity, kMinBloomIntensity, kMaxBloomIntensity, defaults.bloomIntensity);
    settings.bloomScatter =
        ClampFinite(settings.bloomScatter, kMinBloomScatter, kMaxBloomScatter, defaults.bloomScatter);
    return settings;
}

} // namespace Assisi::Render
