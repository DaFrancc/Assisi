/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Bloom.hpp
/// @brief The glow bright light spreads around itself: the ranges of its
/// settings, and the filters that make it. Whether it runs is the player's
/// choice; how strong and how wide is the scene's, in PostProcessSettings.
///
/// Bloom blurs the scene's radiance, wider and wider, and mixes a little of the
/// blur back in before the tone map. Everything glows by the same share, so it
/// shows only around light far brighter than its surroundings: the sun, a lamp,
/// a highlight. There is no brightness threshold.
///
/// The blur is a chain of smaller and smaller copies of the scene, each half the
/// size of the one before, then climbed back up: each copy is blended over the
/// next larger one. **These functions are the specification and
/// bloom_down.frag and bloom_up.frag their transcription**; the tests hold the
/// CPU form.

#include <Assisi/Math/GLM.hpp>

#include <array>
#include <cstdint>

namespace Assisi::Render
{

/// @name Ranges the bloom's settings are held inside
/// @{
/// The share of the blur in the finished picture. Past the ceiling the whole
/// picture is a smear.
inline constexpr float kMinBloomIntensity = 0.0f;
inline constexpr float kMaxBloomIntensity = 0.5f;
inline constexpr float kDefaultBloomIntensity = 0.05f;

/// How much of each copy's glow comes from the smaller, blurrier copies under
/// it rather than from its own size: low keeps the glow tight around a light,
/// high spreads it wide.
inline constexpr float kMinBloomScatter = 0.0f;
inline constexpr float kMaxBloomScatter = 0.95f;
inline constexpr float kDefaultBloomScatter = 0.7f;
/// @}

/// @brief The most copies in the chain, whatever the screen. Enough for an 8K
/// screen to reach the same smallest copy as a 1080p one.
inline constexpr uint32_t kMaxBloomLevels = 10u;

/// @brief How many copies a screen @p height pixels tall blurs through.
///
/// The first is half the screen, and the last is about eight pixels tall,
/// whatever the resolution, so the glow covers the same share of the screen on
/// every display.
[[nodiscard]] uint32_t BloomLevelCount(uint32_t height);

/// @brief The size of copy @p level of a @p width by @p height screen: half
/// the screen at level 0, halving again at each level, and never less than a
/// pixel.
[[nodiscard]] glm::uvec2 BloomLevelExtent(uint32_t width, uint32_t height, uint32_t level);

/// @brief The share of copy @p level, of @p count, in the finished glow: each
/// copy keeps one less @p scatter of itself and takes the rest from the copies
/// under it, so the shares add up to one and an even picture stays even.
[[nodiscard]] float BloomLevelWeight(uint32_t level, uint32_t count, float scatter);

/// @brief A scene sample as the first downsample reads it: not a number becomes
/// black and anything past the largest half float is held to it, so one broken
/// pixel cannot spread over the screen.
[[nodiscard]] glm::vec3 BloomSafeRadiance(const glm::vec3 &radiance);

/// @brief The thirteen samples a downsample takes around a pixel, in source
/// pixels: a 5x5 grid's corners, edge middles and centre, and the four corners
/// of the 3x3 inside it.
///
/// ```
/// a . b . c
/// . j . k .
/// d . e . f
/// . l . m .
/// g . h . i
/// ```
enum class BloomTap : std::uint8_t
{
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    Count_,
};

using BloomTaps = std::array<glm::vec3, static_cast<std::size_t>(BloomTap::Count_)>;

/// @brief One pixel of a copy from the thirteen samples of the copy above it.
///
/// The samples fall in five overlapping 2x2 blocks: the inner one, weighted a
/// half, and one per corner, an eighth each. With @p karis, as the first copy
/// of the scene takes it, each block also counts for less the brighter it is,
/// by one over one plus its brightness, so a single blinding pixel spreads as a
/// soft glow rather than as a large flickering square.
[[nodiscard]] glm::vec3 BloomDownsample(const BloomTaps &taps, bool karis);

/// @brief One pixel climbing back up from the nine samples of the smaller copy
/// around it, weighted as a tent: four for the middle, two for each edge, one
/// for each corner, over sixteen. Row by row, top left first.
[[nodiscard]] glm::vec3 BloomUpsample(const std::array<glm::vec3, 9> &taps);

/// @brief The bloom shaders' push constants. Field order and types match
/// bloom_down.frag's and bloom_up.frag's PushConstants blocks.
struct BloomConstants
{
    /// One pixel of the copy being read, in texture coordinates.
    glm::vec2 texelSize{0.0f};
    /// Climbing up: the share the smaller copy takes over the larger one.
    float scatter = 0.0f;
    /// Going down: 1 for the first copy of the scene, which weights its blocks
    /// by their brightness and guards against broken pixels.
    uint32_t karis = 0u;
};

} // namespace Assisi::Render
