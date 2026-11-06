/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Fog.hpp
/// @brief The level's fog: a haze that thickens with distance and a mist that
/// lies low, both coloured by the sky behind them.
///
/// **These functions are the specification and sky/fog.glsl is their
/// transcription**, as SkyRadiance is for sky.frag. The tests hold the CPU form;
/// the shader has to agree with it.
///
/// Each layer is at full thickness below its height and thins exponentially
/// above it, so a mist fills a valley and the hills above it stand clear. How
/// much a pixel is fogged is the light its line of sight loses crossing both
/// layers, plus a fade over the last stretch before the draw distance, so
/// nothing pops where drawing stops.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/GpuLayout.hpp>
#include <Assisi/Render/Sky.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Assisi::Render
{

/// @brief The optical depth at which fog hides 99% of what is behind it: what a
/// layer reaches over its visibility distance at full thickness. ln(100).
inline constexpr float kFogOpticalDepthAtVisibility = 4.6051702f;

/// @brief The share of the draw distance over which everything fades into the
/// fog's colour however thin the fog is, so the far plane never shows as a wall.
inline constexpr float kFogEdgeFadeFraction = 0.2f;

/// @brief Height spans below this, in metres, are taken as level. The exact
/// integral divides by the span, and over a few millimetres that is dividing
/// rounding by rounding.
inline constexpr float kFogLevelSpan = 0.01f;

/// @brief A falloff of zero gives a layer the same thickness at every height;
/// any other falloff is clamped to this range, in metres.
inline constexpr float kMinFogFalloff = 1.0f;
inline constexpr float kMaxFogFalloff = 100000.0f;

/// @brief The furthest a fog can let the camera see, in metres: the 1,000 km
/// the engine's worlds reach.
inline constexpr float kMaxFogVisibility = 1000000.0f;

/// @brief The highest and lowest a layer's height may be, in metres.
inline constexpr float kMaxFogHeight = 100000.0f;

/// @brief The fog's colour where a level has no sky to take one from.
inline constexpr glm::vec3 kDefaultFogColor{0.5f, 0.55f, 0.6f};

/// @brief The level's fog as an author edits it. Zero visibility turns a layer
/// off; a fog with both layers off still fades the scene out at the draw
/// distance.
struct FogSettings
{
    /// Linear colour, used only where the level has no sky.
    glm::vec3 color = kDefaultFogColor;
    /// How far, in metres, the haze lets the camera see where it is thickest.
    /// Nothing past it is drawn.
    float hazeVisibility = 0.0f;
    /// Below this world height the haze is at its thickest.
    float hazeHeight = 0.0f;
    /// How many metres above @ref hazeHeight the haze takes to thin to about a
    /// third (1/e). Zero keeps it the same at every height.
    float hazeFalloff = 0.0f;
    /// The same three for the mist.
    float mistVisibility = 0.0f;
    float mistHeight = 0.0f;
    float mistFalloff = 0.0f;
};

/// @brief A falloff of zero, or one inside the range.
[[nodiscard]] inline float SanitizedFogFalloff(float falloff)
{
    if (!std::isfinite(falloff) || falloff <= 0.0f)
    {
        return 0.0f;
    }
    return std::clamp(falloff, kMinFogFalloff, kMaxFogFalloff);
}

/// @brief The same settings with every lane finite and in range. Idempotent.
[[nodiscard]] inline FogSettings Sanitized(FogSettings settings)
{
    settings.color = SanitizedSkyChannels(settings.color, kDefaultFogColor);
    settings.hazeVisibility = ClampFiniteSky(settings.hazeVisibility, 0.0f, kMaxFogVisibility, 0.0f);
    settings.hazeHeight = ClampFiniteSky(settings.hazeHeight, -kMaxFogHeight, kMaxFogHeight, 0.0f);
    settings.hazeFalloff = SanitizedFogFalloff(settings.hazeFalloff);
    settings.mistVisibility = ClampFiniteSky(settings.mistVisibility, 0.0f, kMaxFogVisibility, 0.0f);
    settings.mistHeight = ClampFiniteSky(settings.mistHeight, -kMaxFogHeight, kMaxFogHeight, 0.0f);
    settings.mistFalloff = SanitizedFogFalloff(settings.mistFalloff);
    return settings;
}

/// @brief One layer, as the integral wants it.
struct FogLayer
{
    /// Optical depth per metre where the layer is at full thickness.
    float density = 0.0f;
    /// The world height it is full below.
    float height = 0.0f;
    /// Metres above @ref height to thin by a factor of e; zero for even.
    float falloff = 0.0f;
};

/// @brief The density that hides 99% over @p visibility metres, or none.
[[nodiscard]] inline float FogDensityFor(float visibility)
{
    return visibility > 0.0f ? kFogOpticalDepthAtVisibility / visibility : 0.0f;
}

/// @brief Which layer a slot in FogConstants::layers holds.
enum class FogLayerSlot : std::uint8_t
{
    Haze,
    Mist,
    Count_,
};

[[nodiscard]] inline FogLayer HazeLayer(const FogSettings &settings)
{
    return FogLayer{.density = FogDensityFor(settings.hazeVisibility),
                    .height = settings.hazeHeight,
                    .falloff = settings.hazeFalloff};
}

[[nodiscard]] inline FogLayer MistLayer(const FogSettings &settings)
{
    return FogLayer{.density = FogDensityFor(settings.mistVisibility),
                    .height = settings.mistHeight,
                    .falloff = settings.mistFalloff};
}

/// @brief The layer's optical depth per metre at @p height.
[[nodiscard]] inline float FogLayerDensityAt(const FogLayer &layer, float height)
{
    if (layer.falloff <= 0.0f || height <= layer.height)
    {
        return layer.density;
    }
    return layer.density * std::exp((layer.height - height) / layer.falloff);
}

/// @brief The optical depth a straight line of @p length metres collects in
/// @p layer, rising or falling from @p fromHeight to @p toHeight.
///
/// Exact: the part of the line below the layer's height at full density, and
/// the part above it integrated over the exponential, both in height and then
/// scaled to distance by length over the height span.
[[nodiscard]] inline float FogLayerOpticalDepth(const FogLayer &layer, float length, float fromHeight,
                                                float toHeight)
{
    if (layer.density <= 0.0f || length <= 0.0f)
    {
        return 0.0f;
    }
    if (layer.falloff <= 0.0f)
    {
        return layer.density * length;
    }
    const float low = std::min(fromHeight, toHeight);
    const float high = std::max(fromHeight, toHeight);
    const float span = high - low;
    if (span < kFogLevelSpan)
    {
        return length * FogLayerDensityAt(layer, 0.5f * (low + high));
    }
    const float top = std::clamp(layer.height, low, high);
    const float full = layer.density * (top - low);
    // exp(a) - exp(b) as exp(b) * expm1(a - b): over a span far shorter than the
    // falloff the two exponentials are equal to every bit a float has.
    const float thinning = layer.density * layer.falloff * std::exp((layer.height - high) / layer.falloff) *
                           std::expm1((high - top) / layer.falloff);
    return (full + thinning) * (length / span);
}

/// @brief How much of the edge fade applies at @p distance: zero until the last
/// kFogEdgeFadeFraction of @p drawDistance, one at it.
[[nodiscard]] inline float FogEdgeFade(float distance, float drawDistance)
{
    if (!(drawDistance > 0.0f))
    {
        return 0.0f;
    }
    return glm::smoothstep(drawDistance * (1.0f - kFogEdgeFadeFraction), drawDistance, distance);
}

/// @brief How much of a surface @p distance metres away the fog hides, in
/// [0, 1], seen from a camera at @p cameraHeight with the surface at
/// @p pointHeight. One at and past @p drawDistance, whatever the fog's thickness.
[[nodiscard]] inline float FogOpacity(const FogSettings &rawSettings, float distance, float cameraHeight,
                                      float pointHeight, float drawDistance)
{
    const FogSettings settings = Sanitized(rawSettings);
    const float depth = FogLayerOpticalDepth(HazeLayer(settings), distance, cameraHeight, pointHeight) +
                        FogLayerOpticalDepth(MistLayer(settings), distance, cameraHeight, pointHeight);
    const float through = std::exp(-depth) * (1.0f - FogEdgeFade(distance, drawDistance));
    return 1.0f - through;
}

/// @brief How far a camera that would draw to @p farZ draws in this fog: never
/// past the haze's visibility.
[[nodiscard]] inline float FogDrawDistance(float farZ, const FogSettings &rawSettings)
{
    const FogSettings settings = Sanitized(rawSettings);
    return settings.hazeVisibility > 0.0f ? std::min(farZ, settings.hazeVisibility) : farZ;
}

/// @brief The direction whose sky colours the fog along @p direction: itself
/// above the horizon band, and the horizon at the same bearing below it.
///
/// Fog seen below the horizon is air at the horizon's height in front of
/// ground, and the ground the sky draws below the horizon is no part of it.
/// Holding the ray at the top of the band is what makes the fog's colour
/// continuous where it meets the sky.
[[nodiscard]] inline glm::vec3 FogDirection(const glm::vec3 &direction)
{
    const glm::vec3 ray = SafeSkyDirection(direction);
    if (ray.y >= kHorizonSoftness)
    {
        return ray;
    }
    const float level = std::sqrt(1.0f - kHorizonSoftness * kHorizonSoftness);
    const float bearingLength = std::sqrt(ray.x * ray.x + ray.z * ray.z);
    // Straight down has no bearing, and any one is as good as another there.
    if (bearingLength < kMinTangentLength)
    {
        return glm::vec3(level, kHorizonSoftness, 0.0f);
    }
    return glm::vec3(ray.x / bearingLength * level, kHorizonSoftness, ray.z / bearingLength * level);
}

/// @brief The fog's colour along @p direction: the sky's scattered light toward
/// FogDirection, without the sun's disk or the moon's.
[[nodiscard]] inline glm::vec3 FogRadiance(const glm::vec3 &direction, const SkySun &sun, const SkyMoon &rawMoon,
                                           const SkySettings &rawSettings)
{
    SkySettings settings = rawSettings;
    settings.sunDiskIntensity = 0.0f;
    SkyMoon moon = rawMoon;
    moon.diskIntensity = 0.0f;
    return SkyRadiance(FogDirection(direction), sun, moon, settings);
}

/// @brief The fog's lanes in the mesh pass's constant buffer. Mirrors
/// sky/fog.glsl's FogConstants.
struct FogConstants
{
    /// Per FogLayerSlot: x = optical depth per metre at full thickness, y = the
    /// height it is full below, z = falloff in metres (zero for even), w unused.
    std::array<glm::vec4, static_cast<std::size_t>(FogLayerSlot::Count_)> layers{};
    /// x = 1 while the fog is on, y = the camera's world height, z = the draw
    /// distance, w = where the edge fade starts.
    glm::vec4 view{0.0f};
    /// xyz = the colour where there is no sky, w = 1 while the sky gives it.
    glm::vec4 color{0.0f};
    SkyScatterConstants scatter;
};

ASSISI_GPU_LAYOUT(FogConstants);
ASSISI_GPU_FIRST_FIELD(FogConstants, layers);
ASSISI_GPU_FIELD_AFTER(FogConstants, view, layers);
ASSISI_GPU_FIELD_AFTER(FogConstants, color, view);
ASSISI_GPU_FIELD_AFTER(FogConstants, scatter, color);
ASSISI_GPU_NO_TAIL_PADDING(FogConstants, scatter);

/// @brief The constants for one frame's fog. @p scatter is the sky the fog takes
/// its colour from, or null for a level with none.
[[nodiscard]] inline FogConstants MakeFogConstants(const FogSettings &rawSettings, float cameraHeight,
                                                   float drawDistance, const SkyScatterConstants *scatter)
{
    const FogSettings settings = Sanitized(rawSettings);
    const FogLayer haze = HazeLayer(settings);
    const FogLayer mist = MistLayer(settings);

    FogConstants constants;
    constants.layers[static_cast<std::size_t>(FogLayerSlot::Haze)] =
        glm::vec4(haze.density, haze.height, haze.falloff, 0.0f);
    constants.layers[static_cast<std::size_t>(FogLayerSlot::Mist)] =
        glm::vec4(mist.density, mist.height, mist.falloff, 0.0f);
    constants.view = glm::vec4(1.0f, cameraHeight, drawDistance, drawDistance * (1.0f - kFogEdgeFadeFraction));
    constants.color = glm::vec4(settings.color, scatter != nullptr ? 1.0f : 0.0f);
    if (scatter != nullptr)
    {
        constants.scatter = *scatter;
    }
    return constants;
}

} // namespace Assisi::Render
