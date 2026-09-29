/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/CookedBlob.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/AssetKind.hpp>
#include <Assisi/Core/Logger.hpp>

#include <array>
#include <format>
#include <utility>

namespace Assisi::Core
{

namespace
{

constexpr std::array<std::pair<AssetKindId, std::string_view>, 9> kBuiltInKinds{{
    {kReflectedKind, "reflected"},
    {kSceneKind, "scene"},
    {kMeshKind, "mesh"},
    {kTextureKind, "texture"},
    {kShaderKind, "shader"},
    {kVerbatimKind, "verbatim"},
    {kFontKind, "font"},
    {kScreenKind, "screen"},
    {kStringTableKind, "string table"},
}};

} // namespace

std::string_view BuiltInKindName(AssetKindId kind) noexcept
{
    for (const std::pair<AssetKindId, std::string_view> &builtIn : kBuiltInKinds)
    {
        if (builtIn.first == kind)
        {
            return builtIn.second;
        }
    }
    return {};
}

std::string DescribeKind(AssetKindId kind)
{
    const std::string_view builtIn = BuiltInKindName(kind);
    if (!builtIn.empty())
    {
        return std::string{builtIn};
    }
    if (const AssetKind *registered = AssetKindRegistry::Instance().Find(kind); registered != nullptr)
    {
        return registered->name;
    }
    return std::format("unknown kind {:016x}", kind.hash);
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
    default:
        ASSISI_ASSERT(false, "ToString reached a CookedBlobError with no description");
        Log::Error("CookedBlob: no description for this error");
        return "unknown";
    }
}

void WriteCookedHeader(BitWriter &writer, AssetKindId kind)
{
    writer.WriteUInt32(kCookedMagic);
    writer.WriteUInt8(kCookedFormatVersion);
    writer.WriteUInt64(kind.hash);
}

std::expected<AssetKindId, CookedBlobError> ReadCookedHeader(BitReader &reader)
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

    // The version is checked before the kind is read, because an older envelope
    // is shorter and would otherwise be reported as truncated.
    const std::uint8_t version = reader.ReadUInt8();
    if (reader.Failed())
    {
        return std::unexpected(CookedBlobError::Truncated);
    }
    if (version != kCookedFormatVersion)
    {
        return std::unexpected(CookedBlobError::UnsupportedVersion);
    }

    AssetKindId kind;
    kind.hash = reader.ReadUInt64();
    if (reader.Failed())
    {
        return std::unexpected(CookedBlobError::Truncated);
    }
    return kind;
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
