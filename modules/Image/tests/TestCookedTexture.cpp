/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCookedTexture.cpp
/// @brief A cooked texture reads back as the image that was written, and a blob
/// that is not one is refused rather than uploaded.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Image/CookedTexture.hpp>
#include <Assisi/Image/Image.hpp>

using namespace Assisi;

namespace
{

/// An 8x8 BC7 image with its full mip chain, each level filled with a different
/// byte so a level read into the wrong slot fails the comparison.
Image::DecodedImage Bc7Chain()
{
    constexpr std::uint32_t kEdge          = 8;
    constexpr std::size_t kBc7BlockBytes   = 16;
    constexpr std::size_t kLevelBlocks[]   = {4, 1, 1, 1}; // 8x8, 4x4, 2x2, 1x1 texels.

    Image::DecodedImage image;
    image.width      = kEdge;
    image.height     = kEdge;
    image.format     = Image::PixelFormat::Bc7;
    image.colorSpace = Image::ColorSpace::Linear;

    unsigned char fill = 1;
    for (const std::size_t blocks : kLevelBlocks)
    {
        image.mips.emplace_back(blocks * kBc7BlockBytes, fill);
        ++fill;
    }
    return image;
}

std::vector<std::byte> Cook(const Image::DecodedImage &image)
{
    Core::BitWriter writer;
    Image::WriteCookedTexture(writer, image);
    const std::span<const std::byte> bytes = writer.Data();
    return {bytes.begin(), bytes.end()};
}

} // namespace

TEST_CASE("A cooked texture reads back as the image that was written")
{
    const Image::DecodedImage source = Bc7Chain();
    REQUIRE(Image::ValidateMipChain(source));

    const std::expected<Image::DecodedImage, Image::CookedTextureError> read = Image::ReadCookedTexture(Cook(source));
    REQUIRE(read.has_value());
    CHECK(read->width == source.width);
    CHECK(read->height == source.height);
    CHECK(read->format == source.format);
    CHECK(read->colorSpace == source.colorSpace);
    CHECK(read->mips == source.mips);
}

TEST_CASE("A truncated texture blob is refused rather than half-read")
{
    const std::vector<std::byte> bytes = Cook(Bc7Chain());
    for (std::size_t length = 0; length < bytes.size(); ++length)
    {
        CAPTURE(length);
        CHECK_FALSE(Image::ReadCookedTexture(std::span<const std::byte>{bytes.data(), length}).has_value());
    }
}

TEST_CASE("A blob of another kind is not read as a texture")
{
    Core::BitWriter writer;
    Core::WriteCookedHeader(writer, Core::CookedKind::Mesh);
    writer.WriteUInt8(Image::kTexturePayloadVersion);

    const std::expected<Image::DecodedImage, Image::CookedTextureError> read = Image::ReadCookedTexture(writer.Data());
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == Image::CookedTextureError::NotATexture);
}

TEST_CASE("A texture blob of a version this build does not read is refused")
{
    std::vector<std::byte> bytes = Cook(Bc7Chain());
    Core::BitWriter header;
    Core::WriteCookedHeader(header, Core::CookedKind::Texture);
    bytes[header.Data().size()] = std::byte{Image::kTexturePayloadVersion + 1};

    const std::expected<Image::DecodedImage, Image::CookedTextureError> read = Image::ReadCookedTexture(bytes);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == Image::CookedTextureError::UnsupportedVersion);
}

TEST_CASE("A texture blob whose levels do not fit its format is refused")
{
    // Uploading it would copy the pitch the format implies out of a buffer laid
    // out differently, which the graphics API does not catch and samples as noise.
    Image::DecodedImage broken = Bc7Chain();
    broken.mips[1].pop_back();

    const std::expected<Image::DecodedImage, Image::CookedTextureError> read = Image::ReadCookedTexture(Cook(broken));
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == Image::CookedTextureError::Invalid);
}
