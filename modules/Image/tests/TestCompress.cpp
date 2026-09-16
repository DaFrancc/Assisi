/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <Assisi/Image/Compress.hpp>
#include <Assisi/Image/Image.hpp>

using namespace Assisi::Image;

namespace
{
/* How far a decoded BC7 channel may sit from the value that went in, out of 255,
   for a gradient along one axis.

   Tight on purpose. The texels of such a block lie on a line in colour space,
   which is exactly what one BC7 subset encodes, so the format reproduces it to
   within a quantization step. Anything that misreads the block layout — a wrong
   stride, a transposed extraction, an off-by-one row — misses by tens. */
constexpr int32_t kBc7ChannelTolerance = 4;

/* The same, for a single-channel BC4 ramp. BC4 fits two endpoints and six
   interpolated values per block, so a monotonic ramp lands well inside this. */
constexpr int32_t kBc4ChannelTolerance = 8;

/* How far a BC5-encoded normal may tilt from the one that went in, as the cosine
   of the angle between them. Normals are what BC5 exists for and it carries two
   full-precision channels, so anything past this is a broken reconstruction or a
   swapped axis rather than quantization. */
constexpr float kMinNormalDot = 0.99f;

/* A single uncompressed level, so a test can build a source without a chain. */
DecodedImage Rgba8Level(std::uint32_t width, std::uint32_t height, ColorSpace space)
{
    DecodedImage image;
    image.width      = width;
    image.height     = height;
    image.format     = PixelFormat::Rgba8;
    image.colorSpace = space;
    image.mips.resize(1);
    image.mips[0].assign(LayoutFor(PixelFormat::Rgba8, width, height).byteSize, 0u);
    return image;
}

/* A gradient along one axis: every texel of a block sits on one line in colour
   space, which is the shape a single BC7 subset represents exactly. */
DecodedImage GradientImage(std::uint32_t width, std::uint32_t height)
{
    DecodedImage image = Rgba8Level(width, height, ColorSpace::Linear);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::size_t texel  = (static_cast<std::size_t>(y) * width + x) * kRgba8BytesPerTexel;
            const unsigned char ramp = static_cast<unsigned char>(x * 255 / (width > 1 ? width - 1 : 1));
            image.mips[0][texel + 0] = ramp;
            image.mips[0][texel + 1] = ramp;
            image.mips[0][texel + 2] = 128;
            image.mips[0][texel + 3] = 255;
        }
    }
    return image;
}

/* A gradient along both axes at once. The texels of a block then span a plane
   rather than a line, which one subset cannot fit — so this is what separates
   the quality tiers, and it is deliberately the hard case rather than the
   representative one. */
DecodedImage TwoAxisGradientImage(std::uint32_t width, std::uint32_t height)
{
    DecodedImage image = Rgba8Level(width, height, ColorSpace::Linear);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::size_t texel = (static_cast<std::size_t>(y) * width + x) * kRgba8BytesPerTexel;
            image.mips[0][texel + 0] = static_cast<unsigned char>(x * 255 / (width > 1 ? width - 1 : 1));
            image.mips[0][texel + 1] = static_cast<unsigned char>(y * 255 / (height > 1 ? height - 1 : 1));
            image.mips[0][texel + 2] = 128;
            image.mips[0][texel + 3] = 255;
        }
    }
    return image;
}

/* The largest absolute per-channel difference between two RGBA8 levels. */
int32_t WorstChannelError(const DecodedImage &before, const DecodedImage &after)
{
    int32_t worst = 0;
    for (std::size_t byte = 0; byte < before.mips[0].size(); ++byte)
    {
        const int32_t difference =
            std::abs(static_cast<int32_t>(before.mips[0][byte]) - static_cast<int32_t>(after.mips[0][byte]));
        worst = std::max(worst, difference);
    }
    return worst;
}

/* Unit normals fanning across the hemisphere, encoded the way a normal map is:
   each component mapped from [-1, 1] into [0, 255]. */
DecodedImage NormalImage(std::uint32_t width, std::uint32_t height)
{
    DecodedImage image = Rgba8Level(width, height, ColorSpace::Linear);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            // Spread x and y over most of the unit disk, so z stays well clear of
            // zero and the reconstruction is not tested only at its easy centre.
            const float nx = (static_cast<float>(x) / static_cast<float>(width - 1) - 0.5f) * 1.2f;
            const float ny = (static_cast<float>(y) / static_cast<float>(height - 1) - 0.5f) * 1.2f;
            const float nz = std::sqrt(std::max(0.f, 1.f - nx * nx - ny * ny));

            const std::size_t texel = (static_cast<std::size_t>(y) * width + x) * kRgba8BytesPerTexel;
            image.mips[0][texel + 0] = static_cast<unsigned char>(std::lround((nx * 0.5f + 0.5f) * 255.f));
            image.mips[0][texel + 1] = static_cast<unsigned char>(std::lround((ny * 0.5f + 0.5f) * 255.f));
            image.mips[0][texel + 2] = static_cast<unsigned char>(std::lround((nz * 0.5f + 0.5f) * 255.f));
            image.mips[0][texel + 3] = 255;
        }
    }
    return image;
}

/* A stored channel byte back to its signed direction component. */
float ToSigned(unsigned char stored)
{
    return static_cast<float>(stored) / 255.f * 2.f - 1.f;
}
} // namespace

TEST_CASE("Every encoded level is exactly the size its format and dimensions require")
{
    // 5x3 so that neither edge is a multiple of the block extent: the partial
    // blocks are where a missing edge clamp or a wrong block stride shows up.
    DecodedImage source = GradientImage(5, 3);
    source.mips.resize(MipLevelCount(5, 3));
    for (std::uint32_t level = 1; level < source.mips.size(); ++level)
    {
        const LevelLayout layout = LayoutFor(PixelFormat::Rgba8, MipExtent(5, level), MipExtent(3, level));
        source.mips[level].assign(layout.byteSize, 128u);
    }
    REQUIRE(ValidateMipChain(source));

    for (const PixelFormat format : {PixelFormat::Bc4, PixelFormat::Bc5, PixelFormat::Bc7})
    {
        CAPTURE(static_cast<int32_t>(format));
        const auto encoded = Compress(source, format, CompressQuality::Fast);
        REQUIRE(encoded.has_value());

        CHECK(encoded->format == format);
        CHECK(encoded->width == source.width);
        CHECK(encoded->height == source.height);
        // The whole chain survives; a dropped tail level is the failure here.
        REQUIRE(encoded->mips.size() == source.mips.size());
        CHECK(ValidateMipChain(*encoded));

        for (std::uint32_t level = 0; level < encoded->mips.size(); ++level)
        {
            CAPTURE(level);
            const LevelLayout layout = LayoutFor(format, MipExtent(5, level), MipExtent(3, level));
            CHECK(encoded->mips[level].size() == layout.byteSize);
        }
    }
}

TEST_CASE("A BC7 gradient survives the round trip")
{
    const DecodedImage source = GradientImage(16, 16);

    const auto encoded = Compress(source, PixelFormat::Bc7, CompressQuality::Fast);
    REQUIRE(encoded.has_value());
    // Four bytes a texel becoming one byte a texel is the whole point.
    CHECK(encoded->mips[0].size() == source.mips[0].size() / 4);

    const auto decoded = Decompress(*encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->format == PixelFormat::Rgba8);
    REQUIRE(decoded->mips[0].size() == source.mips[0].size());

    // An encoder whose tables were never initialised produces blocks that decode
    // to noise, which no tolerance this tight admits.
    CHECK(WorstChannelError(source, *decoded) <= kBc7ChannelTolerance);
}

TEST_CASE("The best quality tier beats the fast one where the format is under strain")
{
    // A one-axis gradient is reproduced almost exactly at either tier, so it
    // cannot show that the tier is wired to anything. A two-axis one spans a
    // plane that a single subset cannot fit, and closing that gap is precisely
    // what the extra partition search buys.
    const DecodedImage source = TwoAxisGradientImage(16, 16);

    const auto fast = Compress(source, PixelFormat::Bc7, CompressQuality::Fast);
    const auto best = Compress(source, PixelFormat::Bc7, CompressQuality::Best);
    REQUIRE(fast.has_value());
    REQUIRE(best.has_value());
    // Same format, so the tier buys quality and never bytes.
    CHECK(best->mips[0].size() == fast->mips[0].size());

    const auto fastDecoded = Decompress(*fast);
    const auto bestDecoded = Decompress(*best);
    REQUIRE(fastDecoded.has_value());
    REQUIRE(bestDecoded.has_value());

    CHECK(WorstChannelError(source, *bestDecoded) < WorstChannelError(source, *fastDecoded));
}

TEST_CASE("A BC5 normal map survives the round trip and reconstructs its Z")
{
    const DecodedImage source = NormalImage(16, 16);

    const auto encoded = Compress(source, PixelFormat::Bc5, CompressQuality::Fast);
    REQUIRE(encoded.has_value());

    const auto decoded = Decompress(*encoded);
    REQUIRE(decoded.has_value());

    for (std::uint32_t y = 0; y < source.height; ++y)
    {
        for (std::uint32_t x = 0; x < source.width; ++x)
        {
            CAPTURE(x);
            CAPTURE(y);
            const std::size_t texel = (static_cast<std::size_t>(y) * source.width + x) * kRgba8BytesPerTexel;

            const float beforeX = ToSigned(source.mips[0][texel + 0]);
            const float beforeY = ToSigned(source.mips[0][texel + 1]);
            const float beforeZ = ToSigned(source.mips[0][texel + 2]);

            // Exactly what the shader does with a sampled BC5 normal: take the two
            // stored channels and rebuild the third. A swapped X/Y, a wrong block
            // stride, or a reconstruction with the wrong sign all fail here.
            const float afterX = ToSigned(decoded->mips[0][texel + 0]);
            const float afterY = ToSigned(decoded->mips[0][texel + 1]);
            const float afterZ = std::sqrt(std::max(0.f, 1.f - afterX * afterX - afterY * afterY));

            const float dot = beforeX * afterX + beforeY * afterY + beforeZ * afterZ;
            CHECK(dot >= kMinNormalDot);
        }
    }
}

TEST_CASE("A BC4 ramp survives the round trip in its one channel")
{
    DecodedImage source = Rgba8Level(16, 16, ColorSpace::Linear);
    for (std::uint32_t y = 0; y < source.height; ++y)
    {
        for (std::uint32_t x = 0; x < source.width; ++x)
        {
            const std::size_t texel = (static_cast<std::size_t>(y) * source.width + x) * kRgba8BytesPerTexel;
            source.mips[0][texel + 0] = static_cast<unsigned char>(x * 255 / (source.width - 1));
            source.mips[0][texel + 3] = 255;
        }
    }

    const auto encoded = Compress(source, PixelFormat::Bc4, CompressQuality::Fast);
    REQUIRE(encoded.has_value());
    // One channel at 8 bytes a block is half what the 16-byte formats cost.
    CHECK(encoded->mips[0].size() == source.mips[0].size() / 8);

    const auto decoded = Decompress(*encoded);
    REQUIRE(decoded.has_value());

    for (std::uint32_t y = 0; y < source.height; ++y)
    {
        for (std::uint32_t x = 0; x < source.width; ++x)
        {
            const std::size_t texel = (static_cast<std::size_t>(y) * source.width + x) * kRgba8BytesPerTexel;
            const int32_t before    = static_cast<int32_t>(source.mips[0][texel + 0]);
            const int32_t after     = static_cast<int32_t>(decoded->mips[0][texel + 0]);
            CHECK(std::abs(before - after) <= kBc4ChannelTolerance);
        }
    }
}

TEST_CASE("A data format refuses an sRGB source")
{
    // Neither BC4 nor BC5 has an sRGB form. Encoding a normal map as though its
    // values were gamma-encoded corrupts it with nothing to notice, so it is
    // refused rather than silently obeyed.
    const DecodedImage srgb = NormalImage(8, 8);
    DecodedImage srgbTagged = srgb;
    srgbTagged.colorSpace   = ColorSpace::Srgb;

    CHECK_FALSE(Compress(srgbTagged, PixelFormat::Bc5, CompressQuality::Fast).has_value());
    CHECK_FALSE(Compress(srgbTagged, PixelFormat::Bc4, CompressQuality::Fast).has_value());
    // BC7 has one, so the same source is fine there.
    CHECK(Compress(srgbTagged, PixelFormat::Bc7, CompressQuality::Fast).has_value());
}

TEST_CASE("Compress refuses what it cannot encode")
{
    SUBCASE("a source that is already compressed")
    {
        const auto encoded = Compress(GradientImage(8, 8), PixelFormat::Bc7, CompressQuality::Fast);
        REQUIRE(encoded.has_value());
        CHECK_FALSE(Compress(*encoded, PixelFormat::Bc7, CompressQuality::Fast).has_value());
    }

    SUBCASE("a target that is not a block format")
    {
        CHECK_FALSE(Compress(GradientImage(8, 8), PixelFormat::Rgba8, CompressQuality::Fast).has_value());
    }

    SUBCASE("a source whose buffers do not match its own claim")
    {
        DecodedImage broken = GradientImage(8, 8);
        broken.mips[0].resize(broken.mips[0].size() - 1);
        CHECK_FALSE(Compress(broken, PixelFormat::Bc7, CompressQuality::Fast).has_value());
    }
}

TEST_CASE("Decompress refuses an uncompressed source")
{
    CHECK_FALSE(Decompress(GradientImage(8, 8)).has_value());
}
