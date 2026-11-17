/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Tonemap.hpp
/// @brief The look the post chain ends on: white balance, exposure, a tone
/// curve, and a grade.
///
/// The curves themselves are in tonemap.frag. What lives here is everything the
/// CPU has to agree with that shader about — the operator's wire value, the push
/// constants, the ranges a value is allowed to take, and the rule that a
/// copy-through step neither exposes nor grades.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/PostProcessSettings.hpp>

#include <array>
#include <cmath>
#include <cstddef>
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

/// @brief Stops of the player's brightness offset. Beyond this range every
/// scene is either black or white, so the slider stops being a comparison tool.
inline constexpr float kMinExposureStops = -8.0f;
inline constexpr float kMaxExposureStops = 8.0f;

/// @brief The player's part of the look: the curve, a brightness offset, and
/// dither. The level's part is PostProcessSettings.
struct TonemapSettings
{
    TonemapOperator op = TonemapOperator::AgX;
    /// Stops added on top of the exposure the level sets, manual or automatic:
    /// the player's brightness.
    float exposureStops = 0.0f;
    /// Noise of up to one display step before the 8-bit write, which turns the
    /// bands a slow fade steps through into an even grain. See DisplayDither.
    bool dither = true;
};

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
    return settings;
}

/// @brief The linear multiplier @p stops of exposure apply to radiance before
/// the curve.
[[nodiscard]] inline float ExposureScale(float stops)
{
    return std::exp2(ClampFinite(stops, kMinExposureSettingStops, kMaxExposureSettingStops, 0.0f));
}

/// @brief The tone map's push constants. Field order and types match
/// tonemap.frag's PushConstants block.
struct TonemapConstants
{
    /// The white balance matrix's columns, in xyz; w unused.
    std::array<glm::vec4, 3> whiteBalance{glm::vec4(1.0f, 0.0f, 0.0f, 0.0f), glm::vec4(0.0f, 1.0f, 0.0f, 0.0f),
                                          glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)};
    /// The picture is drawn at mix(manualStops, the adapted automatic stops,
    /// automatic) + brightnessStops.
    float manualStops = 0.0f;
    float automatic = 0.0f;
    float brightnessStops = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    std::uint32_t op = static_cast<std::uint32_t>(TonemapOperator::AgX);
    std::uint32_t passthrough = 0;
    /// One display step in output units while dithering, else 0.
    float ditherScale = 0.0f;
    /// The share of the bloom's glow mixed into the radiance before exposure;
    /// 0 reads no glow at all.
    float bloomIntensity = 0.0f;
};

/// @brief The most bytes of push constants every device takes.
inline constexpr std::size_t kMaxPushConstantBytes = 128u;
static_assert(sizeof(TonemapConstants) <= kMaxPushConstantBytes);

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

/// @brief The constants for one tone map draw: the player's @p settings over
/// the scene's @p scene, mixing in @p bloomIntensity of the bloom's glow, zero
/// when no glow was drawn.
///
/// `copyThrough` covers the two steps that must move an image without restating
/// it: the Blit, whose input this same shader already mapped, and the material
/// debug views, whose scene target holds channel values rather than radiance.
/// Both come out as an identity copy — balancing, exposing, grading, dithering
/// or adding glow to either would apply the look a second time.
[[nodiscard]] inline TonemapConstants MakeTonemapConstants(const TonemapSettings &settings,
                                                           const PostProcessSettings &scene, float bloomIntensity,
                                                           bool copyThrough)
{
    if (copyThrough)
    {
        return TonemapConstants{.passthrough = 1};
    }

    const TonemapSettings safe = Sanitized(settings);
    const PostProcessSettings look = Sanitized(scene);
    const glm::mat3 balance = WhiteBalanceMatrix(look.whiteBalanceKelvin, look.whiteBalanceTint);
    return TonemapConstants{
        .whiteBalance = {glm::vec4(balance[0], 0.0f), glm::vec4(balance[1], 0.0f), glm::vec4(balance[2], 0.0f)},
        .manualStops = look.exposure.manualStops,
        .automatic = look.exposure.automatic,
        .brightnessStops = safe.exposureStops,
        .contrast = look.contrast,
        .saturation = look.saturation,
        .op = static_cast<std::uint32_t>(safe.op),
        .passthrough = 0,
        .ditherScale = safe.dither ? 1.0f / kDisplaySteps : 0.0f,
        .bloomIntensity = ClampFinite(bloomIntensity, 0.0f, 1.0f, 0.0f)};
}

} // namespace Assisi::Render
