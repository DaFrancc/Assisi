/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ShadowCascades.hpp>
#include <Assisi/Render/ShadowFiltering.hpp>
#include <Assisi/Render/ShadowSettings.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace Assisi::Render;

namespace
{
/// The two tents the grid filters become.
constexpr float kTent3 = TentHalfWidthTexels(kPcf3FilterRadiusTaps);
constexpr float kTent5 = TentHalfWidthTexels(kPcf5FilterRadiusTaps);

/// Sub-texel positions to sweep, inside [0, 1).
std::vector<float> Fractions()
{
    std::vector<float> fractions;
    for (std::uint32_t i = 0; i < 32u; ++i)
    {
        fractions.push_back(static_cast<float>(i) / 32.f);
    }
    return fractions;
}

/// A tile's worth of texel indices around the lookup, wide enough for either tent.
constexpr std::int32_t kFirstTexel = -4;
constexpr std::int32_t kLastTexel = 5;
} // namespace

TEST_CASE("A tent's texel weights are its integrals over the texels")
{
    // At a texel's centre a 3-wide tent puts five ninths on it and two on each
    // neighbour: the areas of the triangle over those spans, not samples of it.
    CHECK(TentTexelWeight(0.f, kTent3, 0) == doctest::Approx(5.f / 9.f));
    CHECK(TentTexelWeight(0.f, kTent3, -1) == doctest::Approx(2.f / 9.f));
    CHECK(TentTexelWeight(0.f, kTent3, 1) == doctest::Approx(2.f / 9.f));
    CHECK(TentTexelWeight(0.f, kTent3, 2) == doctest::Approx(0.f));

    for (const float halfWidth : {kTent3, kTent5})
    {
        for (const float fraction : Fractions())
        {
            CAPTURE(halfWidth);
            CAPTURE(fraction);
            float sum = 0.f;
            for (std::int32_t texel = kFirstTexel; texel <= kLastTexel; ++texel)
            {
                const float weight = TentTexelWeight(fraction, halfWidth, texel);
                CHECK(weight >= 0.f);
                sum += weight;
            }
            CHECK(sum == doctest::Approx(1.f));
        }
    }
}

TEST_CASE("Bilinear taps reproduce the per-texel tent exactly")
{
    // Every pattern of lit and shadowed texels around the lookup: the taps,
    // each blending the two texels it sits between, must return exactly what
    // weighting every texel by the tent returns. A tap placed at a / (a + b)
    // rather than b / (a + b) leans each pair the wrong way and fails here.
    constexpr std::uint32_t kPatternTexels = static_cast<std::uint32_t>(kLastTexel - kFirstTexel + 1);
    for (const float halfWidth : {kTent3, kTent5})
    {
        for (const float fraction : Fractions())
        {
            const TentAxis axis = TentAxisTaps(fraction, halfWidth);
            CHECK(axis.count == static_cast<std::uint32_t>(halfWidth + 0.5f));
            for (std::uint32_t pattern = 0; pattern < (1u << kPatternTexels); pattern += 7u)
            {
                const auto lit = [&](std::int32_t texel)
                {
                    return static_cast<float>((pattern >> static_cast<std::uint32_t>(texel - kFirstTexel)) & 1u);
                };
                float expected = 0.f;
                for (std::int32_t texel = kFirstTexel; texel <= kLastTexel; ++texel)
                {
                    expected += TentTexelWeight(fraction, halfWidth, texel) * lit(texel);
                }
                float filtered = 0.f;
                for (std::uint32_t i = 0; i < axis.count; ++i)
                {
                    const float below = std::floor(axis.taps[i].position);
                    const float blend = axis.taps[i].position - below;
                    const auto texel = static_cast<std::int32_t>(below);
                    filtered += axis.taps[i].weight * glm::mix(lit(texel), lit(texel + 1), blend);
                }
                CAPTURE(halfWidth);
                CAPTURE(fraction);
                CAPTURE(pattern);
                CHECK(filtered == doctest::Approx(expected));
            }
        }
    }
}

TEST_CASE("A tent reads no further from its lookup than its half-width")
{
    // The tile clamp is inset by this much, and a tap reading past it reads the
    // light in the next tile.
    for (const float halfWidth : {kTent3, kTent5})
    {
        float worst = 0.f;
        for (const float fraction : Fractions())
        {
            const TentAxis axis = TentAxisTaps(fraction, halfWidth);
            for (std::uint32_t i = 0; i < axis.count; ++i)
            {
                if (axis.taps[i].weight > 0.f)
                {
                    // The blend reaches half a texel past the tap on either side.
                    worst = std::max(worst, std::abs(axis.taps[i].position - fraction) + 0.5f);
                }
            }
        }
        CAPTURE(halfWidth);
        CHECK(worst <= halfWidth + 1e-5f);
        CHECK(worst == doctest::Approx(halfWidth));
    }
}

TEST_CASE("A blocker counts only where its own penumbra reaches")
{
    constexpr float kPenumbraPerDepth = 0.1f;
    constexpr float kMaxReach = 1.f;
    constexpr float kOffset = 0.02f;

    // A blocker whose shadow edge falls short of the receiver is not occluding
    // it — the caster's top, seen from its own base.
    const float shortGap = 0.5f * kOffset / kPenumbraPerDepth;
    CHECK_FALSE(PcssBlockerCounts(kPenumbraPerDepth, shortGap, kOffset, kMaxReach));
    // The same place with a penumbra wide enough to reach: it counts.
    const float reachingGap = 2.f * kOffset / kPenumbraPerDepth;
    CHECK(PcssBlockerCounts(kPenumbraPerDepth, reachingGap, kOffset, kMaxReach));
    // Nothing in front of the receiver is no blocker, however close.
    CHECK_FALSE(PcssBlockerCounts(kPenumbraPerDepth, 0.f, 0.f, kMaxReach));
    CHECK_FALSE(PcssBlockerCounts(kPenumbraPerDepth, -0.1f, 0.f, kMaxReach));
}

TEST_CASE("The blocker search never reaches less than the floor kernel")
{
    constexpr float kFloorReach = 0.003f;
    constexpr float kMaxReach = 0.05f;
    constexpr float kReference = 0.8f;

    // A point source: the search is sized from a zero penumbra, and without the
    // floor it looks at one texel and calls every soft edge lit.
    CHECK(PcssSearchRadiusUv(0.f, kReference, kMaxReach, kFloorReach) == doctest::Approx(kFloorReach));
    // A wide source searches as far as its widest penumbra.
    CHECK(PcssSearchRadiusUv(0.05f, kReference, kMaxReach, kFloorReach) == doctest::Approx(0.04f));
    CHECK(PcssSearchRadiusUv(1.f, kReference, kMaxReach, kFloorReach) == doctest::Approx(kMaxReach));
}

TEST_CASE("The contact-hardening disk takes more taps as it widens, and never past the cap")
{
    constexpr float kFloorReach = 0.01f;
    // Inside the floor the tent answers, and the disk takes nothing.
    CHECK(PcssKernelTaps(0.f, kFloorReach) == 0u);
    CHECK(PcssKernelTaps(kFloorReach, kFloorReach) == 0u);
    // Just past it, the disk's fewest; far past it, its most.
    CHECK(PcssKernelTaps(kFloorReach * 1.01f, kFloorReach) == kPcssMinTaps);
    CHECK(PcssKernelTaps(kFloorReach * 100.f, kFloorReach) == kPcssMaxTaps);

    // Monotone and evenly divisible by the probe, so the step from one count to
    // the next is a few taps rather than a cliff.
    std::uint32_t previous = kPcssMinTaps;
    for (float reach = kFloorReach * 1.01f; reach < kFloorReach * 4.f; reach *= 1.05f)
    {
        const std::uint32_t taps = PcssKernelTaps(reach, kFloorReach);
        CHECK(taps >= previous);
        CHECK(taps % kPcssProbeTaps == 0u);
        CHECK(taps - previous <= kPcssProbeTaps * 2u);
        previous = taps;
    }
}

TEST_CASE("The footprint bias covers every texel a comparison reads")
{
    constexpr float kTexelUv = 1.f / 512.f;
    for (const glm::vec2 slope : {glm::vec2(0.f), glm::vec2(3.f, 0.f), glm::vec2(-2.f, 5.f), glm::vec2(10.f, -10.f)})
    {
        const float bound = CompareFootprintDepth(kTexelUv, slope);
        for (const glm::vec2 corner : {glm::vec2(1.f, 1.f), glm::vec2(-1.f, 1.f), glm::vec2(1.f, -1.f), glm::vec2(-1.f, -1.f)})
        {
            CAPTURE(slope);
            CHECK(bound >= std::abs(glm::dot(corner * kTexelUv * kCompareFootprintTexels, slope)) - 1e-9f);
        }
    }
    // A flat receiver needs none.
    CHECK(CompareFootprintDepth(kTexelUv, glm::vec2(0.f)) == 0.f);
}
