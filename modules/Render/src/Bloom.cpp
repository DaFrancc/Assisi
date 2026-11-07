/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/Bloom.hpp>

#include <bit>
#include <cmath>

namespace Assisi::Render
{
namespace
{

// The smallest copy is about two to this power pixels tall.
constexpr uint32_t kSmallestLevelLog2 = 3u;

// The largest finite half float, the scene target's format.
constexpr float kMaxHalfFloat = 65504.0f;

constexpr glm::vec3 kLumaWeights{0.2126f, 0.7152f, 0.0722f};

// The inner block's share of a downsample, and each corner block's.
constexpr float kInnerBlockWeight = 0.5f;
constexpr float kCornerBlockWeight = 0.125f;

// The tent's weights over its total.
constexpr float kTentMiddle = 4.0f;
constexpr float kTentEdge = 2.0f;
constexpr float kTentCorner = 1.0f;
constexpr float kTentTotal = 16.0f;

glm::vec3 Tap(const BloomTaps &taps, BloomTap tap)
{
    return taps[static_cast<std::size_t>(tap)];
}

glm::vec3 BlockAverage(const BloomTaps &taps, BloomTap a, BloomTap b, BloomTap c, BloomTap d)
{
    return 0.25f * (Tap(taps, a) + Tap(taps, b) + Tap(taps, c) + Tap(taps, d));
}

} // namespace

uint32_t BloomLevelCount(uint32_t height)
{
    if (height < 2u)
    {
        return 1u;
    }
    // Copy n is the height over 2^(n+1), so with this many the last is between
    // 2^kSmallestLevelLog2 and twice that tall.
    const uint32_t log2 = static_cast<uint32_t>(std::bit_width(height)) - 1u;
    const uint32_t levels = log2 > kSmallestLevelLog2 ? log2 - kSmallestLevelLog2 : 1u;
    return std::clamp(levels, 1u, kMaxBloomLevels);
}

glm::uvec2 BloomLevelExtent(uint32_t width, uint32_t height, uint32_t level)
{
    const uint32_t shift = std::min(level + 1u, 31u);
    return glm::max(glm::uvec2(width >> shift, height >> shift), glm::uvec2(1u));
}

float BloomLevelWeight(uint32_t level, uint32_t count, float scatter)
{
    if (count == 0u || level >= count)
    {
        return 0.0f;
    }
    const float reached = std::pow(scatter, static_cast<float>(level));
    return level + 1u == count ? reached : reached * (1.0f - scatter);
}

glm::vec3 BloomSafeRadiance(const glm::vec3 &radiance)
{
    glm::vec3 safe(0.0f);
    for (glm::length_t channel = 0; channel < 3; ++channel)
    {
        const float value = radiance[channel];
        // A comparison with not-a-number is false, so it stays at zero.
        safe[channel] = value > 0.0f ? std::min(value, kMaxHalfFloat) : 0.0f;
    }
    return safe;
}

glm::vec3 BloomDownsample(const BloomTaps &taps, bool karis)
{
    const std::array<glm::vec3, 5> blocks{
        BlockAverage(taps, BloomTap::J, BloomTap::K, BloomTap::L, BloomTap::M),
        BlockAverage(taps, BloomTap::A, BloomTap::B, BloomTap::D, BloomTap::E),
        BlockAverage(taps, BloomTap::B, BloomTap::C, BloomTap::E, BloomTap::F),
        BlockAverage(taps, BloomTap::D, BloomTap::E, BloomTap::G, BloomTap::H),
        BlockAverage(taps, BloomTap::E, BloomTap::F, BloomTap::H, BloomTap::I),
    };
    glm::vec3 sum(0.0f);
    float total = 0.0f;
    for (std::size_t i = 0; i < blocks.size(); ++i)
    {
        const float share = i == 0 ? kInnerBlockWeight : kCornerBlockWeight;
        const float weight = karis ? share / (1.0f + glm::dot(blocks[i], kLumaWeights)) : share;
        sum += blocks[i] * weight;
        total += weight;
    }
    return sum / total;
}

glm::vec3 BloomUpsample(const std::array<glm::vec3, 9> &taps)
{
    const glm::vec3 corners = taps[0] + taps[2] + taps[6] + taps[8];
    const glm::vec3 edges = taps[1] + taps[3] + taps[5] + taps[7];
    return (taps[4] * kTentMiddle + edges * kTentEdge + corners * kTentCorner) / kTentTotal;
}

} // namespace Assisi::Render
