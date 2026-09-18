/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CookedPayload.hpp
/// @brief The shader, verbatim and reflected blobs: writers and readers kept
///        together so each layout has one definition.
///
/// Meshes and textures have their own (Geometry/CookedMesh.hpp,
/// Image/CookedTexture.hpp) because their payloads are those modules' types.
/// These three need nothing beyond Core.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <typeindex>
#include <vector>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Reflect/AssetTypeMeta.hpp>
#include <Assisi/Core/Reflect/AssetTypeRegistry.hpp>

namespace Assisi::Core
{

/// @brief Why bytes did not read as the payload asked for.
enum class CookedPayloadError : std::uint8_t
{
    NotCooked,   ///< Not a cooked blob, or an envelope this build does not read.
    WrongKind,   ///< A cooked blob of another kind.
    WrongType,   ///< A reflected blob holding a different type, or a type this build does not reflect.
    Truncated,   ///< The bytes end, or a length claims more than is left.
    Undecodable, ///< Framed correctly, but the fields do not decode against this build's layout.
};

/// @brief A short human-readable description, for a load failure's log line.
[[nodiscard]] std::string_view ToString(CookedPayloadError error) noexcept;

/// @brief Write @p spirv as a complete shader blob.
void WriteShaderBlob(BitWriter &writer, std::span<const std::byte> spirv);

/// @brief The SPIR-V a shader blob holds.
[[nodiscard]] std::expected<std::vector<std::byte>, CookedPayloadError> ReadShaderBlob(std::span<const std::byte> bytes);

/// @brief Write @p contents as a complete verbatim blob.
void WriteVerbatimBlob(BitWriter &writer, std::span<const std::byte> contents);

/// @brief The bytes a verbatim blob holds.
[[nodiscard]] std::expected<std::vector<std::byte>, CookedPayloadError>
ReadVerbatimBlob(std::span<const std::byte> bytes);

/// @brief Write @p instance as a complete reflected blob: its type name, then its
///        fields through the asset codec.
///
/// @return false if a field cannot be encoded; the writer holds a partial blob
///         and must be discarded.
[[nodiscard]] bool WriteReflectedBlob(BitWriter &writer, const Reflect::AssetTypeMeta &meta, const void *instance);

/// @brief Read a reflected blob into @p instance, which must be of @p meta's type.
///
/// The stored type name is checked against @p meta before any field is read. On
/// failure @p instance may hold some fields, so read into a scratch value.
[[nodiscard]] std::expected<void, CookedPayloadError>
ReadReflectedBlob(std::span<const std::byte> bytes, const Reflect::AssetTypeMeta &meta, void *instance);

/// @brief ReadReflectedBlob for a type known to the compiler, starting from a
///        default-constructed value.
template <typename T>
[[nodiscard]] std::expected<T, CookedPayloadError> ReadReflectedBlob(std::span<const std::byte> bytes)
{
    const Reflect::AssetTypeMeta *meta = Reflect::AssetTypeRegistry::Instance().Find(std::type_index(typeid(T)));
    if (meta == nullptr)
    {
        return std::unexpected(CookedPayloadError::WrongType);
    }
    T value{};
    if (const std::expected<void, CookedPayloadError> read = ReadReflectedBlob(bytes, *meta, &value); !read)
    {
        return std::unexpected(read.error());
    }
    return value;
}

} // namespace Assisi::Core
