/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/CookedPayload.hpp>

#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>

#include <string>

namespace Assisi::Core
{

namespace
{

constexpr std::size_t kBitsPerByte = 8;

/// Reads and checks the envelope, leaving @p reader at the payload.
std::expected<void, CookedPayloadError> ExpectKind(BitReader &reader, CookedKind wanted)
{
    const std::expected<CookedKind, CookedBlobError> kind = ReadCookedHeader(reader);
    if (!kind)
    {
        return std::unexpected(kind.error() == CookedBlobError::Truncated ? CookedPayloadError::Truncated
                                                                          : CookedPayloadError::NotCooked);
    }
    if (*kind != wanted)
    {
        return std::unexpected(CookedPayloadError::WrongKind);
    }
    return {};
}

void WriteByteBlob(BitWriter &writer, CookedKind kind, std::span<const std::byte> contents)
{
    WriteCookedHeader(writer, kind);
    writer.WriteVarUInt32(static_cast<std::uint32_t>(contents.size()));
    writer.WriteBytes(contents);
}

std::expected<std::vector<std::byte>, CookedPayloadError> ReadByteBlob(std::span<const std::byte> bytes,
                                                                        CookedKind kind)
{
    BitReader reader{bytes};
    if (const std::expected<void, CookedPayloadError> envelope = ExpectKind(reader, kind); !envelope)
    {
        return std::unexpected(envelope.error());
    }

    const std::uint32_t size = reader.ReadVarUInt32();
    if (reader.Failed() || size > reader.BitsRemaining() / kBitsPerByte)
    {
        return std::unexpected(CookedPayloadError::Truncated);
    }
    std::vector<std::byte> contents(size);
    reader.ReadBytes(contents);
    if (reader.Failed())
    {
        return std::unexpected(CookedPayloadError::Truncated);
    }
    return contents;
}

} // namespace

std::string_view ToString(CookedPayloadError error) noexcept
{
    switch (error)
    {
    case CookedPayloadError::NotCooked:
        return "not a cooked blob this build reads";
    case CookedPayloadError::WrongKind:
        return "a cooked blob of another kind";
    case CookedPayloadError::WrongType:
        return "a reflected blob of another type";
    case CookedPayloadError::Truncated:
        return "the bytes end part-way through";
    case CookedPayloadError::Undecodable:
        return "the fields do not decode against this build's layout";
    }
    return "unknown";
}

void WriteShaderBlob(BitWriter &writer, std::span<const std::byte> spirv)
{
    WriteByteBlob(writer, CookedKind::Shader, spirv);
}

std::expected<std::vector<std::byte>, CookedPayloadError> ReadShaderBlob(std::span<const std::byte> bytes)
{
    return ReadByteBlob(bytes, CookedKind::Shader);
}

void WriteVerbatimBlob(BitWriter &writer, std::span<const std::byte> contents)
{
    WriteByteBlob(writer, CookedKind::Verbatim, contents);
}

std::expected<std::vector<std::byte>, CookedPayloadError> ReadVerbatimBlob(std::span<const std::byte> bytes)
{
    return ReadByteBlob(bytes, CookedKind::Verbatim);
}

bool WriteReflectedBlob(BitWriter &writer, const Reflect::AssetTypeMeta &meta, const void *instance)
{
    WriteCookedHeader(writer, CookedKind::Reflected);
    writer.WriteString(meta.name);
    return Reflect::WriteAsset(meta, instance, writer);
}

std::expected<void, CookedPayloadError> ReadReflectedBlob(std::span<const std::byte> bytes,
                                                          const Reflect::AssetTypeMeta &meta, void *instance)
{
    BitReader reader{bytes};
    if (const std::expected<void, CookedPayloadError> envelope = ExpectKind(reader, CookedKind::Reflected); !envelope)
    {
        return std::unexpected(envelope.error());
    }

    const std::string typeName = reader.ReadString();
    if (reader.Failed())
    {
        return std::unexpected(CookedPayloadError::Truncated);
    }
    if (typeName != meta.name)
    {
        return std::unexpected(CookedPayloadError::WrongType);
    }

    if (!Reflect::ReadAsset(meta, instance, reader))
    {
        return std::unexpected(reader.Failed() ? CookedPayloadError::Truncated : CookedPayloadError::Undecodable);
    }
    return {};
}

} // namespace Assisi::Core
