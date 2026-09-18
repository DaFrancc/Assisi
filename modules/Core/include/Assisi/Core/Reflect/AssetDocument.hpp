/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/AssetDocument.hpp
/// @brief The `{ "version", "type", <fields> }` envelope every reflected asset
///        type is stored in, as text.
///
/// An AASSET's generated code produces and consumes a flat object of its fields
/// and nothing else. This layer is what turns that into a file: it writes the
/// two envelope keys above the payload, and on the way back it refuses a
/// document whose `type` names a different asset type before any field reaches
/// the instance. Without that check a `.amat` loaded as a config would apply
/// whatever field names happened to coincide and default the rest, which reads
/// as a corrupt file rather than the wrong one.
///
/// Pure text in, text out — no filesystem — so the caller chooses where the
/// bytes come from: the asset root, the writable user root, or a string in a
/// test.
///
/// Fields absent from a document keep the value the instance already holds.
/// That is what makes this the layering primitive as well as the loader: apply
/// a shipped document to a default-constructed instance, then apply a user's
/// document to the same instance, and the result is default-then-override with
/// no merge code anywhere.

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <typeindex>

namespace Assisi::Core::Reflect
{

/// @brief Why reading or writing an asset document failed.
enum class AssetDocumentError : std::uint8_t
{
    NotRegistered, ///< The type's generated reflection object is not in this link.
    ParseFailed,   ///< The text is not a JSON object.
    WrongType,     ///< The envelope's `type` names a different asset type.
    BadField,      ///< A field is present but unreadable; the instance is now partly written.
};

[[nodiscard]] std::string_view ToString(AssetDocumentError error) noexcept;

/// @brief Schema version written into every document this build produces.
///
/// One number for all asset types rather than one per type. Nothing reads it
/// yet: it exists so a future format change has a value to branch on, and a
/// single counter is enough for that because the envelope is what it describes,
/// not the fields inside — those are already forward- and backward-compatible
/// through the absent-key rule.
inline constexpr std::int32_t kAssetDocumentVersion = 1;

/// @brief Apply @p text to @p instance, leaving fields the document omits alone.
///
/// @param text     A document produced by SerializeAssetDocument.
/// @param type     The asset type @p instance points at.
/// @param instance A mutable instance of that type.
///
/// On BadField the instance may already carry the fields that preceded the bad
/// one, so a caller that cannot tolerate a half-applied value should apply to a
/// scratch instance and commit only on success.
std::expected<void, AssetDocumentError> ApplyAssetDocument(std::string_view text, std::type_index type,
                                                           void *instance);

/// @brief @p instance as a document: the envelope, then its fields flat beneath.
std::expected<std::string, AssetDocumentError> SerializeAssetDocument(std::type_index type, const void *instance);

/// @brief ApplyAssetDocument for a known static type.
template <typename T> std::expected<void, AssetDocumentError> ApplyAssetDocument(std::string_view text, T &instance)
{
    return ApplyAssetDocument(text, std::type_index(typeid(T)), &instance);
}

/// @brief SerializeAssetDocument for a known static type.
template <typename T> std::expected<std::string, AssetDocumentError> SerializeAssetDocument(const T &instance)
{
    return SerializeAssetDocument(std::type_index(typeid(T)), &instance);
}

} // namespace Assisi::Core::Reflect
