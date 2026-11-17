/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Exposure.hpp
/// @brief How bright the picture is drawn: a fixed exposure, or one that
/// follows the scene as eyes adjust to it.
///
/// Automatic exposure counts the scene's pixels by brightness each frame, into
/// a histogram of stops, and takes the average brightness of the pixels between
/// two shares of the count, so a few very dark or very bright pixels do not
/// swing it. It then aims to draw that average at a set brightness, held inside a
/// range, and moves toward the aim a little each frame, so stepping from
/// daylight into a dark room brightens the picture over a second or two.
///
/// **These functions are the specification and exposure_histogram.comp and
/// exposure_adapt.comp their transcription**; the tests hold the CPU form.

#include <Assisi/Math/Float.hpp>
#include <Assisi/Math/GLM.hpp>

#include <algorithm>
#include <cstdint>
#include <span>

namespace Assisi::Render
{

/// @brief How many bins the brightness histogram has. Bin 0 holds the pixels
/// too dark to count, which metering leaves out; the rest split the range below
/// evenly.
inline constexpr uint32_t kExposureHistogramBins = 128u;

/// @brief The brightness range the histogram covers, as log2 of luminance: from
/// a moonless night to well past the sun on snow.
inline constexpr float kExposureHistogramMinLog2 = -14.0f;
inline constexpr float kExposureHistogramMaxLog2 = 10.0f;

/// @brief Metering leaves out the darkest and brightest of the counted pixels:
/// the share below the first and above the second.
inline constexpr float kExposureMeterLowShare = 0.5f;
inline constexpr float kExposureMeterHighShare = 0.95f;

/// @brief The luminance automatic exposure draws the metered average at.
///
/// Measured rather than photography's mid grey of 0.18: the engine's lights and
/// sky are scaled so a daylit scene looks right with no exposure at all, and
/// metering takes the brighter half of a frame, so a daylit test scene, its
/// sky included, meters about 2.2 stops above mid grey. This draws it as no
/// exposure does.
inline constexpr float kExposureTargetLuminance = 0.8f;

/// @name Ranges the exposure settings are held inside, in stops
/// @{
inline constexpr float kMinExposureSettingStops = -16.0f;
inline constexpr float kMaxExposureSettingStops = 16.0f;
/// How fast automatic exposure closes on its aim: per second, the share of the
/// way left it covers is one less e to minus this. Zero never moves.
inline constexpr float kMinExposureAdaptationRate = 0.0f;
inline constexpr float kMaxExposureAdaptationRate = 20.0f;
/// @}

/// @name What an automatic exposure starts with
/// @{
inline constexpr float kDefaultAutoExposureMinStops = -4.0f;
inline constexpr float kDefaultAutoExposureMaxStops = 8.0f;
inline constexpr float kDefaultExposureAdaptationRate = 1.5f;
/// @}

/// @brief The exposure a frame is drawn at, as the scene's volumes blended it.
///
/// A share of it may be automatic: the picture is drawn at the manual stops
/// where @ref automatic is 0, at the adapted automatic stops where it is 1, and
/// between the two in stops between.
struct ExposureSettings
{
    float manualStops = 0.0f;
    /// From 0, wholly manual, to 1, wholly automatic.
    float automatic = 0.0f;
    /// The automatic exposure never goes past these.
    float minStops = kDefaultAutoExposureMinStops;
    float maxStops = kDefaultAutoExposureMaxStops;
    /// Added to the automatic aim: positive draws brighter.
    float compensationStops = 0.0f;
    float adaptationRate = kDefaultExposureAdaptationRate;

    bool operator==(const ExposureSettings &) const = default;
};

/// @brief The same settings with every lane in range, the minimum no more than
/// the maximum. Idempotent.
[[nodiscard]] ExposureSettings Sanitized(ExposureSettings settings);

/// @brief The bin a pixel of @p luminance falls in: 0 for one too dark to
/// count, otherwise 1 to kExposureHistogramBins - 1, the brightest bin also
/// taking everything past the range.
[[nodiscard]] uint32_t ExposureHistogramBin(float luminance);

/// @brief log2 of the luminance in the middle of @p bin, for bins 1 and up.
[[nodiscard]] float ExposureHistogramBinLog2(uint32_t bin);

/// @brief The average brightness of a frame, as log2 of luminance, from its
/// histogram: over the counted pixels between kExposureMeterLowShare and
/// kExposureMeterHighShare of them, bin 0 left out, a bin straddling either
/// share counting only its part inside. The target's, so no exposure, when
/// nothing was counted.
[[nodiscard]] float MeteredLog2Luminance(std::span<const uint32_t> bins);

/// @brief The stops automatic exposure aims at for a frame metered at
/// @p meteredLog2: those that draw it at kExposureTargetLuminance, plus the compensation, held
/// inside the settings' range.
[[nodiscard]] float TargetExposureStops(float meteredLog2, const ExposureSettings &settings);

/// @brief The exposure one frame of @p seconds later, moving from @p current
/// toward @p target at @p rate.
[[nodiscard]] float AdaptExposureStops(float current, float target, float rate, float seconds);

/// @brief What the automatic exposure keeps on the GPU between frames. Mirrors
/// exposure_adapt.comp's and tonemap.frag's ExposureState.
struct ExposureState
{
    /// The adapted automatic exposure.
    float stops = 0.0f;
    /// 0 until the first frame has metered, which takes its aim at once rather
    /// than adapting toward it from nothing.
    uint32_t seeded = 0u;
};

/// @brief The exposure passes' push constants. Field order and types match
/// exposure_histogram.comp's and exposure_adapt.comp's PushConstants blocks.
struct ExposureConstants
{
    /// The grid of samples the histogram takes, one per 2x2 block of pixels.
    glm::uvec2 sampleGrid{0u};
    /// One pixel of the scene, in texture coordinates.
    glm::vec2 texelSize{0.0f};
    float minStops = 0.0f;
    float maxStops = 0.0f;
    float compensationStops = 0.0f;
    float adaptationRate = 0.0f;
    /// How long the frame took, in seconds.
    float seconds = 0.0f;
};

} // namespace Assisi::Render
