/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/Reflect/AssetDocument.hpp>

#include <Assisi/Core/Reflect/AssetTypeRegistry.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>

namespace Assisi::Core::Reflect
{
namespace
{
/// The envelope keys, named once so the reader and the writer cannot drift.
constexpr const char *kVersionKey = "version";
constexpr const char *kTypeKey    = "type";

/// Indent width of a written document. These files are read and hand-edited by
/// people, so they are written pretty rather than compact.
constexpr int32_t kIndentSpaces = 2;
} // namespace

std::string_view ToString(AssetDocumentError error) noexcept
{
    switch (error)
    {
    case AssetDocumentError::NotRegistered:
        return "asset type reflection not registered";
    case AssetDocumentError::ParseFailed:
        return "not a JSON object";
    case AssetDocumentError::WrongType:
        return "wrong asset type";
    case AssetDocumentError::BadField:
        return "a field could not be read";
    }
    return "unknown error";
}

std::expected<void, AssetDocumentError> ApplyAssetDocument(std::string_view text, std::type_index type, void *instance)
{
    const AssetTypeMeta *meta = AssetTypeRegistry::Instance().Find(type);
    if (meta == nullptr)
    {
        return std::unexpected(AssetDocumentError::NotRegistered);
    }

    // allow_exceptions=false: nlohmann throws on malformed input, and a parse
    // error here is an ordinary outcome for a hand-edited file, not something to
    // unwind through.
    const nlohmann::json document = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/ false);
    if (document.is_discarded() || !document.is_object())
    {
        return std::unexpected(AssetDocumentError::ParseFailed);
    }

    // Before any field is applied. A document that names another type may still
    // share field names with this one, and half-applying it would leave an
    // instance nobody can account for.
    if (document.value(kTypeKey, std::string{}) != meta->name)
    {
        return std::unexpected(AssetDocumentError::WrongType);
    }

    // The envelope keys are not fields, so they are simply not looked for. Keys
    // the type does not declare are ignored the same way.
    if (!meta->deserialize(document, instance))
    {
        return std::unexpected(AssetDocumentError::BadField);
    }

    return {};
}

std::expected<std::string, AssetDocumentError> SerializeAssetDocument(std::type_index type, const void *instance)
{
    const AssetTypeMeta *meta = AssetTypeRegistry::Instance().Find(type);
    if (meta == nullptr)
    {
        return std::unexpected(AssetDocumentError::NotRegistered);
    }

    // Envelope first so version and type read at the top of the file; the
    // reflected payload is merged in flat beneath them rather than nested, so a
    // field is spelled the same in the file as it is in the struct.
    nlohmann::json document;
    document[kVersionKey] = kAssetDocumentVersion;
    document[kTypeKey]    = meta->name;

    const nlohmann::json fields = meta->serialize(instance);
    for (const auto &[key, value] : fields.items())
    {
        document[key] = value;
    }

    return document.dump(kIndentSpaces);
}

} // namespace Assisi::Core::Reflect
