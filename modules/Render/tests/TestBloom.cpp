/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Bloom.hpp>
#include <Assisi/Render/Tonemap.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using namespace Assisi::Render;

namespace
{

constexpr glm::vec3 kLumaWeights{0.2126f, 0.7152f, 0.0722f};

BloomTaps Even(const glm::vec3 &colour)
{
    BloomTaps taps;
    taps.fill(colour);
    return taps;
}

// Black but for one tap.
BloomTaps Lone(BloomTap tap, const glm::vec3 &colour)
{
    BloomTaps taps = Even(glm::vec3(0.0f));
    taps[static_cast<std::size_t>(tap)] = colour;
    return taps;
}

void CheckNear(const glm::vec3 &actual, const glm::vec3 &expected)
{
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.z == doctest::Approx(expected.z));
}

} // namespace

TEST_CASE("The chain ends about eight pixels tall at any resolution")
{
    CHECK(BloomLevelCount(1080u) == 7u);
    CHECK(BloomLevelExtent(1920u, 1080u, 0u) == glm::uvec2(960u, 540u));
    CHECK(BloomLevelExtent(1920u, 1080u, 6u) == glm::uvec2(15u, 8u));
    CHECK(BloomLevelCount(2160u) == 8u);
    CHECK(BloomLevelExtent(3840u, 2160u, 7u).y == 8u);

    for (const uint32_t height : {64u, 480u, 720u, 1080u, 1440u, 1600u, 2160u, 4320u})
    {
        CAPTURE(height);
        const uint32_t last = BloomLevelExtent(height, height, BloomLevelCount(height) - 1u).y;
        CHECK(last >= 8u);
        CHECK(last < 16u);
    }
}

TEST_CASE("A tiny or enormous screen still gets a chain the targets can hold")
{
    for (const uint32_t height : {1u, 2u, 7u, 8u, 16u})
    {
        CAPTURE(height);
        CHECK(BloomLevelCount(height) >= 1u);
        CHECK(BloomLevelExtent(height, height, BloomLevelCount(height) - 1u).y >= 1u);
    }
    CHECK(BloomLevelCount(1u << 20u) == kMaxBloomLevels);
    CHECK(BloomLevelExtent(3u, 3u, 5u) == glm::uvec2(1u));
}

TEST_CASE("The copies' shares of the glow add up to one, so an even picture stays even")
{
    for (const uint32_t count : {1u, 2u, 5u, 7u, kMaxBloomLevels})
    {
        for (const float scatter : {0.0f, 0.3f, kDefaultBloomScatter, kMaxBloomScatter})
        {
            CAPTURE(count);
            CAPTURE(scatter);
            float total = 0.0f;
            for (uint32_t level = 0; level < count; ++level)
            {
                total += BloomLevelWeight(level, count, scatter);
            }
            CHECK(total == doctest::Approx(1.0f));
        }
    }
    CHECK(BloomLevelWeight(0u, 4u, 0.0f) == doctest::Approx(1.0f));
    CHECK(BloomLevelWeight(3u, 4u, 0.0f) == doctest::Approx(0.0f));
    CHECK(BloomLevelWeight(4u, 4u, 0.5f) == 0.0f);
}

TEST_CASE("The shares are what climbing the chain back up blends")
{
    // BloomPass blends each copy over the next larger one by the scatter, from
    // the smallest up. Each copy holding only its own level's light, the first
    // copy ends with each level's share.
    constexpr uint32_t kCount = 6u;
    constexpr float kScatter = 0.6f;
    std::vector<std::array<float, kCount>> copies(kCount);
    for (uint32_t level = 0; level < kCount; ++level)
    {
        copies[level].fill(0.0f);
        copies[level][level] = 1.0f;
    }
    for (uint32_t i = kCount - 1u; i-- > 0u;)
    {
        for (uint32_t level = 0; level < kCount; ++level)
        {
            copies[i][level] = kScatter * copies[i + 1u][level] + (1.0f - kScatter) * copies[i][level];
        }
    }
    for (uint32_t level = 0; level < kCount; ++level)
    {
        CAPTURE(level);
        CHECK(copies[0][level] == doctest::Approx(BloomLevelWeight(level, kCount, kScatter)));
    }
}

TEST_CASE("A downsample of an even picture is the same colour")
{
    const glm::vec3 colour(0.3f, 2.0f, 40.0f);
    CheckNear(BloomDownsample(Even(colour), /*karis=*/ false), colour);
    CheckNear(BloomDownsample(Even(colour), /*karis=*/ true), colour);
}

TEST_CASE("A downsample weights its taps as the thirteen-tap filter does")
{
    // The centre and the inner four count an eighth each, the edge middles a
    // sixteenth, the corners a thirty-second: the sum of five 2x2 blocks.
    const glm::vec3 one(1.0f);
    CheckNear(BloomDownsample(Lone(BloomTap::E, one), false), glm::vec3(0.125f));
    CheckNear(BloomDownsample(Lone(BloomTap::J, one), false), glm::vec3(0.125f));
    CheckNear(BloomDownsample(Lone(BloomTap::B, one), false), glm::vec3(0.0625f));
    CheckNear(BloomDownsample(Lone(BloomTap::A, one), false), glm::vec3(0.03125f));
    CheckNear(BloomDownsample(Lone(BloomTap::I, one), false), glm::vec3(0.03125f));
}

TEST_CASE("The first downsample keeps one blinding pixel from becoming a blinding square")
{
    // A pixel ten thousand times brighter than white, alone among black ones.
    // Plain, an eighth of it reaches the copy, and every copy after spreads
    // that wider: a large square flickering as the pixel moves. Weighted by
    // brightness, its blocks count for so little that it comes out about as
    // bright as white.
    const glm::vec3 blinding(10000.0f);
    for (const BloomTap tap : {BloomTap::E, BloomTap::J, BloomTap::A})
    {
        CAPTURE(static_cast<int32_t>(tap));
        CHECK(glm::dot(BloomDownsample(Lone(tap, blinding), false), kLumaWeights) > 300.0f);
        const float weighted = glm::dot(BloomDownsample(Lone(tap, blinding), true), kLumaWeights);
        CHECK(weighted > 0.0f);
        CHECK(weighted < 2.0f);
    }
}

TEST_CASE("Weighting by brightness barely changes a picture with nothing blinding in it")
{
    BloomTaps taps = Even(glm::vec3(0.0f));
    for (std::size_t i = 0; i < taps.size(); ++i)
    {
        taps[i] = glm::vec3(0.01f + 0.001f * static_cast<float>(i));
    }
    const float plain = glm::dot(BloomDownsample(taps, false), kLumaWeights);
    const float weighted = glm::dot(BloomDownsample(taps, true), kLumaWeights);
    CHECK(weighted == doctest::Approx(plain).epsilon(0.01));
}

TEST_CASE("Climbing up reads the smaller copy through a tent")
{
    std::array<glm::vec3, 9> taps{};
    taps.fill(glm::vec3(0.5f, 1.0f, 3.0f));
    CheckNear(BloomUpsample(taps), glm::vec3(0.5f, 1.0f, 3.0f));

    taps.fill(glm::vec3(0.0f));
    taps[4] = glm::vec3(1.0f);
    CheckNear(BloomUpsample(taps), glm::vec3(0.25f));
    taps[4] = glm::vec3(0.0f);
    taps[1] = glm::vec3(1.0f);
    CheckNear(BloomUpsample(taps), glm::vec3(0.125f));
    taps[1] = glm::vec3(0.0f);
    taps[8] = glm::vec3(1.0f);
    CheckNear(BloomUpsample(taps), glm::vec3(0.0625f));
}

TEST_CASE("A broken scene pixel reaches the chain as something finite")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    CheckNear(BloomSafeRadiance(glm::vec3(nan, 1.0f, -2.0f)), glm::vec3(0.0f, 1.0f, 0.0f));
    CheckNear(BloomSafeRadiance(glm::vec3(infinity, 0.5f, 3.0f)), glm::vec3(65504.0f, 0.5f, 3.0f));
}

TEST_CASE("A scene's bloom settings are held in range")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const PostProcessSettings broken = Sanitized(PostProcessSettings{.bloomIntensity = nan, .bloomScatter = nan});
    CHECK(broken.bloomIntensity == kDefaultBloomIntensity);
    CHECK(broken.bloomScatter == kDefaultBloomScatter);

    const PostProcessSettings loud = Sanitized(PostProcessSettings{.bloomIntensity = 9.0f, .bloomScatter = 2.0f});
    CHECK(loud.bloomIntensity == kMaxBloomIntensity);
    CHECK(loud.bloomScatter == kMaxBloomScatter);
}

TEST_CASE("The tone map mixes in the glow it is given, and none when it only copies")
{
    const TonemapSettings settings;
    const PostProcessSettings scene;
    CHECK(MakeTonemapConstants(settings, scene, 0.1f, /*copyThrough=*/ false).bloomIntensity == doctest::Approx(0.1f));
    CHECK(MakeTonemapConstants(settings, scene, 0.1f, /*copyThrough=*/ true).bloomIntensity == 0.0f);
    CHECK(MakeTonemapConstants(settings, scene, std::numeric_limits<float>::quiet_NaN(), false).bloomIntensity ==
          0.0f);
}
