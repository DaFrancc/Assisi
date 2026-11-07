/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CloudQuality.hpp
/// @brief How much detail the player's machine spends on the clouds.
///
/// What the clouds look like is the level's (Render::CloudSettings); how finely
/// they are drawn is the player's. Both knobs trade detail for time without
/// changing what is in the sky.

#include <algorithm>
#include <bit>
#include <cstdint>

namespace Assisi::Render
{

/// @brief The sky map's width in texels; its height is a quarter of that. The
/// map stands for a whole turn of sky, so at the default a texel is about a
/// third of a degree, a few pixels of a 1080p screen: the clouds' edges are
/// soft, which suits them.
inline constexpr uint32_t kMinCloudMapResolution = 256;
inline constexpr uint32_t kMaxCloudMapResolution = 4096;
inline constexpr uint32_t kDefaultCloudMapResolution = 2048;

/// @brief Steps along a ray straight up through the clouds; a ray toward the
/// horizon crosses more of them and takes up to three times as many. Fewer is
/// faster and shows as blotches inside the clouds. Each texel of the map
/// averages nine rays staggered across one step, so a ray needs far fewer
/// steps than it would alone: past about 24, more steps change little, where a
/// larger map sharpens every cloud.
inline constexpr uint32_t kMinCloudSteps = 8;
inline constexpr uint32_t kMaxCloudSteps = 256;
inline constexpr uint32_t kDefaultCloudSteps = 24;

struct CloudQuality
{
    uint32_t resolution = kDefaultCloudMapResolution;
    uint32_t steps = kDefaultCloudSteps;

    bool operator==(const CloudQuality &) const = default;
};

/// @brief @p quality in range, with the resolution rounded down to a power of
/// two so the map's height divides evenly.
[[nodiscard]] inline CloudQuality Sanitized(CloudQuality quality)
{
    quality.resolution =
        std::bit_floor(std::clamp(quality.resolution, kMinCloudMapResolution, kMaxCloudMapResolution));
    quality.steps = std::clamp(quality.steps, kMinCloudSteps, kMaxCloudSteps);
    return quality;
}

/// @brief Named points in the clouds' quality, as the shadows have. Custom is
/// what any other combination reads as, not a choice.
enum class CloudTier : std::uint8_t
{
    Low,
    Medium,
    High,
    Ultra,
    Custom,
};

/// @brief The quality a tier stands for. High is the default. The tiers spend
/// on the map's size rather than on steps, which is where the detail is; each
/// is about twice to three times the one below.
[[nodiscard]] inline CloudQuality CloudTierSettings(CloudTier tier)
{
    switch (tier)
    {
    case CloudTier::Low:
        return CloudQuality{.resolution = 512, .steps = 16};
    case CloudTier::Medium:
        return CloudQuality{.resolution = 1024, .steps = 24};
    case CloudTier::High:
    case CloudTier::Custom:
        break;
    case CloudTier::Ultra:
        return CloudQuality{.resolution = 4096, .steps = 24};
    }
    return CloudQuality{};
}

/// @brief Which tier @p quality is, or Custom.
[[nodiscard]] inline CloudTier Tier(const CloudQuality &quality)
{
    for (CloudTier tier : {CloudTier::Low, CloudTier::Medium, CloudTier::High, CloudTier::Ultra})
    {
        if (Sanitized(quality) == CloudTierSettings(tier))
        {
            return tier;
        }
    }
    return CloudTier::Custom;
}

} // namespace Assisi::Render
