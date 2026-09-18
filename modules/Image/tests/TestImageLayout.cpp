/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Image/Image.hpp>

using namespace Assisi::Image;

namespace
{
/* A chain of the right shape for `format`, every level filled with a byte so the
   sizes are the only thing under test. */
DecodedImage ChainOf(PixelFormat format, std::uint32_t width, std::uint32_t height)
{
    DecodedImage image;
    image.width      = width;
    image.height     = height;
    image.format     = format;
    image.colorSpace = ColorSpace::Linear;

    const std::uint32_t levels = MipLevelCount(width, height);
    image.mips.resize(levels);
    for (std::uint32_t level = 0; level < levels; ++level)
    {
        const LevelLayout layout = LayoutFor(format, MipExtent(width, level), MipExtent(height, level));
        image.mips[level].assign(layout.byteSize, 0u);
    }
    return image;
}
} // namespace

TEST_CASE("A block-compressed level rounds its width up to a whole block")
{
    // The tail of every mip chain is narrower than one block, so this is the
    // ordinary case rather than an edge one.
    CHECK(LayoutFor(PixelFormat::Bc7, 1, 1).rowPitch == 16);
    CHECK(LayoutFor(PixelFormat::Bc7, 2, 1).rowPitch == 16);
    CHECK(LayoutFor(PixelFormat::Bc7, 3, 1).rowPitch == 16);
    CHECK(LayoutFor(PixelFormat::Bc7, 4, 1).rowPitch == 16);
    // Five texels need two blocks, not one and a quarter.
    CHECK(LayoutFor(PixelFormat::Bc7, 5, 1).rowPitch == 32);

    // An 8-byte format disagrees with the RGBA8 arithmetic at every width, which
    // is what makes it the one that catches a pitch computed the wrong way.
    CHECK(LayoutFor(PixelFormat::Bc4, 1, 1).rowPitch == 8);
    CHECK(LayoutFor(PixelFormat::Bc4, 4, 1).rowPitch == 8);
    CHECK(LayoutFor(PixelFormat::Bc4, 8, 1).rowPitch == 16);
}

TEST_CASE("A block-compressed level rounds its height up to a whole block row")
{
    // One block row covers four texel rows, so heights 1 through 4 are one row.
    CHECK(LayoutFor(PixelFormat::Bc7, 4, 1).byteSize == 16);
    CHECK(LayoutFor(PixelFormat::Bc7, 4, 4).byteSize == 16);
    CHECK(LayoutFor(PixelFormat::Bc7, 4, 5).byteSize == 32);
    CHECK(LayoutFor(PixelFormat::Bc7, 8, 8).byteSize == 4 * 16);
}

TEST_CASE("An uncompressed level is four bytes a texel, addressed by row")
{
    CHECK(LayoutFor(PixelFormat::Rgba8, 3, 1).rowPitch == 12);
    CHECK(LayoutFor(PixelFormat::Rgba8, 3, 2).byteSize == 24);
    CHECK(BytesPerBlock(PixelFormat::Rgba8) == 0);
    CHECK_FALSE(IsBlockCompressed(PixelFormat::Rgba8));
}

TEST_CASE("The uncompressed pitch agrees with the block pitch exactly where it must not be trusted")
{
    // The reason the tail levels above are the test that matters: for a 16-byte
    // format at a width divisible by four, the wrong arithmetic gives the right
    // answer, so every level of a power-of-two texture agrees until the last two.
    for (std::uint32_t width = 4; width <= 2048; width *= 2)
    {
        CHECK(LayoutFor(PixelFormat::Bc7, width, 1).rowPitch == LayoutFor(PixelFormat::Rgba8, width, 1).rowPitch);
    }
    // And here is where it stops agreeing.
    CHECK(LayoutFor(PixelFormat::Bc7, 2, 1).rowPitch != LayoutFor(PixelFormat::Rgba8, 2, 1).rowPitch);
    CHECK(LayoutFor(PixelFormat::Bc4, 4, 1).rowPitch != LayoutFor(PixelFormat::Rgba8, 4, 1).rowPitch);
}

TEST_CASE("A mip chain runs from the base down to 1x1")
{
    CHECK(MipLevelCount(1, 1) == 1);
    CHECK(MipLevelCount(256, 256) == 9);
    CHECK(MipLevelCount(2048, 2048) == 12);
    // A non-square image keeps halving until both edges reach one.
    CHECK(MipLevelCount(8, 2) == 4);
    CHECK(MipExtent(8, 3) == 1);
    CHECK(MipExtent(2048, 11) == 1);
}

TEST_CASE("A chain is valid only when every level is the size its format implies")
{
    CHECK(ValidateMipChain(ChainOf(PixelFormat::Bc7, 16, 16)));
    CHECK(ValidateMipChain(ChainOf(PixelFormat::Bc5, 5, 3)));
    CHECK(ValidateMipChain(ChainOf(PixelFormat::Rgba8, 7, 9)));

    SUBCASE("a level sized for RGBA8 but labelled BC7 is refused")
    {
        // The failure the graphics API cannot report: it would read the BC7 pitch
        // out of a buffer laid out for RGBA8 and sample the result as noise.
        DecodedImage mislabelled = ChainOf(PixelFormat::Rgba8, 16, 16);
        mislabelled.format       = PixelFormat::Bc7;
        CHECK_FALSE(ValidateMipChain(mislabelled));
    }

    SUBCASE("a truncated chain is still valid")
    {
        // A downsample that fails drops the levels below it, which is deliberate.
        DecodedImage truncated = ChainOf(PixelFormat::Bc7, 16, 16);
        truncated.mips.resize(2);
        CHECK(ValidateMipChain(truncated));
    }

    SUBCASE("more levels than the dimensions allow is refused")
    {
        DecodedImage overlong = ChainOf(PixelFormat::Bc7, 16, 16);
        overlong.mips.emplace_back(16, std::uint8_t{0});
        CHECK_FALSE(ValidateMipChain(overlong));
    }

    SUBCASE("an empty chain is refused")
    {
        DecodedImage empty;
        empty.width  = 4;
        empty.height = 4;
        CHECK_FALSE(ValidateMipChain(empty));
    }
}
