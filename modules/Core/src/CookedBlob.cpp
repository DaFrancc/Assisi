/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/CookedBlob.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>

namespace Assisi::Core
{

std::string_view ToString(CookedKind kind) noexcept
{
    switch (kind)
    {
    case CookedKind::Reflected:
        return "reflected";
    case CookedKind::Scene:
        return "scene";
    case CookedKind::Mesh:
        return "mesh";
    case CookedKind::Texture:
        return "texture";
    case CookedKind::Shader:
        return "shader";
    case CookedKind::Verbatim:
        return "verbatim";
    case CookedKind::Font:
        return "font";
    default:
        ASSISI_ASSERT(false, "ToString reached a CookedKind with no name");
        Log::Error("CookedBlob: no name for this kind");
        return "unknown";
    }
}

std::string_view ToString(CookedBlobError error) noexcept
{
    switch (error)
    {
    case CookedBlobError::Truncated:
        return "too short to hold a cooked header";
    case CookedBlobError::BadMagic:
        return "not a cooked blob";
    case CookedBlobError::UnsupportedVersion:
        return "a cooked format version this build does not read";
    case CookedBlobError::UnknownKind:
        return "a cooked kind this build does not have";
    default:
        ASSISI_ASSERT(false, "ToString reached a CookedBlobError with no description");
        Log::Error("CookedBlob: no description for this error");
        return "unknown";
    }
}

void WriteCookedHeader(BitWriter &writer, CookedKind kind)
{
    ASSISI_ASSERT(kind < CookedKind::Count, "WriteCookedHeader: kind is not a real enumerator");

    writer.WriteUInt32(kCookedMagic);
    writer.WriteUInt8(kCookedFormatVersion);
    writer.WriteUInt8(static_cast<std::uint8_t>(kind));
}

std::expected<CookedKind, CookedBlobError> ReadCookedHeader(BitReader &reader)
{
    const std::uint32_t magic = reader.ReadBits(32);
    // Checked before the magic comparison, because a failed read returns zero
    // and would otherwise be reported as the wrong kind of wrong file.
    if (reader.Failed())
    {
        return std::unexpected(CookedBlobError::Truncated);
    }
    if (magic != kCookedMagic)
    {
        return std::unexpected(CookedBlobError::BadMagic);
    }

    const std::uint32_t version = reader.ReadBits(8);
    const std::uint32_t kind    = reader.ReadBits(8);
    if (reader.Failed())
    {
        return std::unexpected(CookedBlobError::Truncated);
    }

    if (version != kCookedFormatVersion)
    {
        return std::unexpected(CookedBlobError::UnsupportedVersion);
    }
    if (kind >= static_cast<std::uint32_t>(CookedKind::Count))
    {
        return std::unexpected(CookedBlobError::UnknownKind);
    }
    return static_cast<CookedKind>(kind);
}

void WriteAssetId(BitWriter &writer, const AssetId &id)
{
    for (const std::uint8_t byte : id.bytes)
    {
        writer.WriteUInt8(byte);
    }
}

AssetId ReadAssetId(BitReader &reader)
{
    AssetId id;
    for (std::uint8_t &byte : id.bytes)
    {
        byte = reader.ReadUInt8();
    }
    return id;
}

} // namespace Assisi::Core
