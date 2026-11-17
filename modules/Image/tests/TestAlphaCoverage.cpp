/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAlphaCoverage.cpp
/// @brief A cutout texture's mips keep the share of texels the alpha test keeps,
///        so cut-out grass does not thin out with distance.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <Assisi/Image/Decode.hpp>
#include <Assisi/Image/Image.hpp>

using namespace Assisi::Image;

namespace
{
/// A grass card: thin solid blades, one to three texels wide, rising from the
/// bottom edge to random heights at random places, over empty texels.
constexpr std::uint32_t kSide = 256;
constexpr std::uint32_t kBlades = 40;
constexpr std::uint32_t kMaxBladeWidth = 3;
constexpr float kCutoff = 0.5f;
/// How far a level's coverage may stray from the base's: coverage moves in
/// steps, one texel or one alpha value at a time.
constexpr float kCoverageSlack = 0.02f;
/// Two ways of reaching the same share may round it differently.
constexpr float kFloatSlack = 1e-6f;
constexpr std::size_t kRgba = 4;
constexpr std::size_t kAlpha = 3;
constexpr unsigned char kSolid = 255;

/// A fixed sequence of pseudo-random numbers, so the card is the same every run
/// (Numerical Recipes' linear congruential constants).
class Sequence
{
public:
    std::uint32_t Next(std::uint32_t below)
    {
        _state = _state * 1664525u + 1013904223u;
        return (_state >> 8) % below;
    }

private:
    std::uint32_t _state = 1;
};

DecodedImage GrassCard()
{
    std::vector<unsigned char> base(static_cast<std::size_t>(kSide) * kSide * kRgba, 0);
    Sequence random;
    for (std::uint32_t blade = 0; blade < kBlades; ++blade)
    {
        const std::uint32_t left = random.Next(kSide - kMaxBladeWidth);
        const std::uint32_t width = 1 + random.Next(kMaxBladeWidth);
        const std::uint32_t top = random.Next(kSide);
        for (std::uint32_t y = top; y < kSide; ++y)
        {
            for (std::uint32_t x = left; x < left + width; ++x)
            {
                const std::size_t texel = (static_cast<std::size_t>(y) * kSide + x) * kRgba;
                base[texel + 1] = kSolid; // green
                base[texel + kAlpha] = kSolid;
            }
        }
    }
    DecodedImage image;
    image.width = kSide;
    image.height = kSide;
    image.colorSpace = ColorSpace::Srgb;
    BuildMipChain(image, base.data(), /*generateMips=*/ true);
    return image;
}

/// The share of @p mip's texels the alpha test keeps at @p cutoff.
float Coverage(const std::vector<unsigned char> &mip, float cutoff)
{
    std::size_t kept = 0;
    for (std::size_t i = kAlpha; i < mip.size(); i += kRgba)
    {
        kept += static_cast<float>(mip[i]) / 255.f >= cutoff ? 1u : 0u;
    }
    return static_cast<float>(kept) / static_cast<float>(mip.size() / kRgba);
}

/// Whether @p mip has texels enough for its coverage to be held to the slack.
bool FineEnough(const std::vector<unsigned char> &mip)
{
    return 1.f / static_cast<float>(mip.size() / kRgba) <= kCoverageSlack;
}
} // namespace

TEST_CASE("Averaged mips lose a cutout's thin blades, which is what the rescale is for")
{
    const DecodedImage plain = GrassCard();
    const float base = Coverage(plain.mips[0], kCutoff);
    REQUIRE(base > 0.f);
    // Three halvings average each blade with the empty texels beside it.
    REQUIRE(plain.mips.size() > 3);
    CHECK(Coverage(plain.mips[3], kCutoff) < base / 2.f);
}

TEST_CASE("Every mip of a cutout texture keeps its base level's coverage at the cutoff")
{
    DecodedImage image = GrassCard();
    const float base = Coverage(image.mips[0], kCutoff);
    PreserveAlphaCoverage(image, kCutoff);
    for (std::size_t level = 1; level < image.mips.size() && FineEnough(image.mips[level]); ++level)
    {
        CAPTURE(level);
        CHECK(std::abs(Coverage(image.mips[level], kCutoff) - base) <= kCoverageSlack);
    }
}

TEST_CASE("The rescale follows the cutoff it is given")
{
    // A higher cutoff cuts more of an averaged mip, so it needs a larger scale
    // to keep the same coverage; either way the coverage is the base's.
    constexpr float kHighCutoff = 0.8f;
    DecodedImage image = GrassCard();
    const float base = Coverage(image.mips[0], kHighCutoff);
    PreserveAlphaCoverage(image, kHighCutoff);
    for (std::size_t level = 1; level < image.mips.size() && FineEnough(image.mips[level]); ++level)
    {
        CAPTURE(level);
        CHECK(std::abs(Coverage(image.mips[level], kHighCutoff) - base) <= kCoverageSlack);
    }
}

TEST_CASE("Each mip's coverage is the nearest to the base's that any scale gives")
{
    // A scale keeps every texel at or above some alpha, so the coverages it can
    // give are the shares of texels at or above each alpha the level holds.
    // Coverage moves in those steps, so it seldom lands on the base's exactly;
    // the nearer step is the answer.
    const DecodedImage plain = GrassCard();
    DecodedImage scaled = plain;
    PreserveAlphaCoverage(scaled, kCutoff);
    const float wanted = Coverage(plain.mips[0], kCutoff);

    for (std::size_t level = 1; level < plain.mips.size(); ++level)
    {
        const std::vector<unsigned char> &mip = plain.mips[level];
        const float texels = static_cast<float>(mip.size() / kRgba);
        float nearest = wanted; // keeping nothing
        for (int threshold = 1; threshold <= kSolid; ++threshold)
        {
            std::size_t kept = 0;
            for (std::size_t i = kAlpha; i < mip.size(); i += kRgba)
            {
                kept += mip[i] >= threshold ? 1u : 0u;
            }
            nearest = std::min(nearest, std::abs(static_cast<float>(kept) / texels - wanted));
        }
        CAPTURE(level);
        CHECK(std::abs(Coverage(scaled.mips[level], kCutoff) - wanted) <= nearest + kFloatSlack);
    }
}

TEST_CASE("The rescale touches alpha alone, and the base level not at all")
{
    const DecodedImage before = GrassCard();
    DecodedImage after = before;
    PreserveAlphaCoverage(after, kCutoff);

    CHECK(after.mips[0] == before.mips[0]);
    bool alphaChanged = false;
    for (std::size_t level = 1; level < after.mips.size(); ++level)
    {
        for (std::size_t i = 0; i < after.mips[level].size(); ++i)
        {
            if (i % kRgba == kAlpha)
            {
                alphaChanged = alphaChanged || after.mips[level][i] != before.mips[level][i];
            }
            else
            {
                CHECK(after.mips[level][i] == before.mips[level][i]);
            }
        }
    }
    CHECK(alphaChanged);
}

TEST_CASE("No cutoff, or a base level the cutoff cuts whole, is left as it is")
{
    const DecodedImage before = GrassCard();
    DecodedImage noCutoff = before;
    PreserveAlphaCoverage(noCutoff, 0.f);
    CHECK(noCutoff.mips == before.mips);

    // Above every alpha a byte holds, so nothing at the base is kept.
    constexpr float kAboveEverything = 1.5f;
    DecodedImage allCut = before;
    PreserveAlphaCoverage(allCut, kAboveEverything);
    CHECK(allCut.mips == before.mips);
}
