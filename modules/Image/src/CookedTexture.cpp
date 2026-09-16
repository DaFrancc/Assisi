/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Image/CookedTexture.hpp>

#include <Assisi/Core/CookedBlob.hpp>

#include <vector>

namespace Assisi::Image
{

namespace
{

constexpr std::size_t kBitsPerByte = 8;

/// A mip level's length prefix is a varint, so a level takes at least this.
constexpr std::size_t kMinLevelBytes = 1;

/// Bytes left in @p reader.
std::size_t BytesLeft(const Core::BitReader &reader)
{
    return reader.BitsRemaining() / kBitsPerByte;
}

} // namespace

std::string_view ToString(CookedTextureError error) noexcept
{
    switch (error)
    {
    case CookedTextureError::NotATexture:
        return "not a cooked texture";
    case CookedTextureError::UnsupportedVersion:
        return "a texture layout this build does not read";
    case CookedTextureError::Truncated:
        return "the texture bytes end part-way through";
    case CookedTextureError::Invalid:
        return "the texture's levels do not fit its format and size";
    }
    return "unknown";
}

void WriteCookedTexture(Core::BitWriter &writer, const DecodedImage &image)
{
    Core::WriteCookedHeader(writer, Core::CookedKind::Texture);
    writer.WriteUInt8(kTexturePayloadVersion);
    writer.WriteUInt32(image.width);
    writer.WriteUInt32(image.height);
    writer.WriteUInt8(static_cast<std::uint8_t>(image.format));
    writer.WriteUInt8(static_cast<std::uint8_t>(image.colorSpace));
    writer.WriteVarUInt32(static_cast<std::uint32_t>(image.mips.size()));
    for (const std::vector<unsigned char> &mip : image.mips)
    {
        writer.WriteVarUInt32(static_cast<std::uint32_t>(mip.size()));
        writer.WriteBytes(std::as_bytes(std::span{mip}));
    }
}

std::expected<DecodedImage, CookedTextureError> ReadCookedTexture(std::span<const std::byte> bytes)
{
    Core::BitReader reader{bytes};

    const std::expected<Core::CookedKind, Core::CookedBlobError> kind = Core::ReadCookedHeader(reader);
    if (!kind || *kind != Core::CookedKind::Texture)
    {
        if (!kind && kind.error() == Core::CookedBlobError::Truncated)
        {
            return std::unexpected(CookedTextureError::Truncated);
        }
        return std::unexpected(CookedTextureError::NotATexture);
    }

    const std::uint8_t version = reader.ReadUInt8();
    if (reader.Failed())
    {
        return std::unexpected(CookedTextureError::Truncated);
    }
    if (version != kTexturePayloadVersion)
    {
        return std::unexpected(CookedTextureError::UnsupportedVersion);
    }

    DecodedImage image;
    image.width                    = reader.ReadUInt32();
    image.height                   = reader.ReadUInt32();
    const std::uint8_t format      = reader.ReadUInt8();
    const std::uint8_t colorSpace  = reader.ReadUInt8();
    const std::uint32_t levelCount = reader.ReadVarUInt32();
    if (reader.Failed() || levelCount > BytesLeft(reader) / kMinLevelBytes)
    {
        return std::unexpected(CookedTextureError::Truncated);
    }
    if (format >= static_cast<std::uint8_t>(PixelFormat::Count) ||
        colorSpace > static_cast<std::uint8_t>(ColorSpace::Linear))
    {
        return std::unexpected(CookedTextureError::Invalid);
    }
    image.format     = static_cast<PixelFormat>(format);
    image.colorSpace = static_cast<ColorSpace>(colorSpace);

    image.mips.resize(levelCount);
    for (std::vector<unsigned char> &mip : image.mips)
    {
        const std::uint32_t levelBytes = reader.ReadVarUInt32();
        if (reader.Failed() || levelBytes > BytesLeft(reader))
        {
            return std::unexpected(CookedTextureError::Truncated);
        }
        mip.resize(levelBytes);
        reader.ReadBytes(std::as_writable_bytes(std::span{mip}));
    }
    if (reader.Failed())
    {
        return std::unexpected(CookedTextureError::Truncated);
    }

    if (!ValidateMipChain(image))
    {
        return std::unexpected(CookedTextureError::Invalid);
    }
    return image;
}

} // namespace Assisi::Image
