/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Render/BlueNoise.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>

using namespace Assisi::Render;

namespace
{
constexpr std::uint32_t kLevels = 256;

/// The mean absolute difference between neighbouring values of uniform white
/// noise on [0, 1] is a third. Blue noise pushes neighbours apart, and the
/// generated tile measures about 0.41; this margin is well clear of white noise
/// and well inside what the tile has.
constexpr float kWhiteNoiseNeighbourDifference = 1.f / 3.f;
constexpr float kBlueNeighbourMargin = 0.05f;
} // namespace

TEST_CASE("The blue-noise tile holds every value equally often")
{
    // A rotation drawn from it is then uniform over the turn, so no angle of the
    // shadow disk is favoured anywhere on screen.
    const std::span<const std::uint8_t> tile = BlueNoiseTile();
    REQUIRE(tile.size() == kBlueNoiseTileSize * kBlueNoiseTileSize);

    std::array<std::uint32_t, kLevels> counts{};
    for (const std::uint8_t value : tile)
    {
        ++counts[value];
    }
    const std::uint32_t expected = static_cast<std::uint32_t>(tile.size()) / kLevels;
    for (const std::uint32_t count : counts)
    {
        CHECK(count == expected);
    }
}

TEST_CASE("Neighbouring texels of the blue-noise tile differ more than white noise's")
{
    // What makes it blue: energy at high frequencies. A table corrupted into
    // white noise, or sorted, or blurred, all fall below this.
    const std::span<const std::uint8_t> tile = BlueNoiseTile();
    const std::uint32_t size = kBlueNoiseTileSize;
    float total = 0.f;
    for (std::uint32_t y = 0; y < size; ++y)
    {
        for (std::uint32_t x = 0; x < size; ++x)
        {
            const std::int32_t here = tile[y * size + x];
            total += static_cast<float>(std::abs(here - tile[y * size + (x + 1u) % size]));
            total += static_cast<float>(std::abs(here - tile[((y + 1u) % size) * size + x]));
        }
    }
    const float mean = total / (2.f * static_cast<float>(size * size) * static_cast<float>(kLevels - 1u));
    CHECK(mean > kWhiteNoiseNeighbourDifference + kBlueNeighbourMargin);
}
