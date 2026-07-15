/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/AssetSidecar.hpp>

#include <nlohmann/json.hpp>

namespace Assisi::Core
{

std::string_view ToString(AssetSidecarError error) noexcept
{
    switch (error)
    {
    case AssetSidecarError::ParseFailed:
        return "invalid JSON";
    case AssetSidecarError::WrongType:
        return "wrong asset type";
    case AssetSidecarError::MissingGuid:
        return "missing or malformed guid";
    }
    return "unknown error";
}

std::string SerializeSidecar(const AssetSidecar &sidecar)
{
    // Envelope first so "version"/"type" read at the top of the file, matching
    // the `.amat` layout. The id is a canonical UUID string.
    nlohmann::json document;
    document["version"] = kAssetSidecarVersion;
    document["type"]    = std::string(kAssetSidecarType);
    document["guid"]    = sidecar.guid.ToString();
    return document.dump(2);
}

std::expected<AssetSidecar, AssetSidecarError> DeserializeSidecar(std::string_view jsonText)
{
    const nlohmann::json document = nlohmann::json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object())
    {
        return std::unexpected(AssetSidecarError::ParseFailed);
    }

    if (document.value("type", std::string{}) != kAssetSidecarType)
    {
        return std::unexpected(AssetSidecarError::WrongType);
    }

    const std::string guidText = document.value("guid", std::string{});
    const std::optional<AssetId> guid = AssetId::Parse(guidText);
    if (!guid.has_value())
    {
        return std::unexpected(AssetSidecarError::MissingGuid);
    }

    return AssetSidecar{.guid = *guid};
}

} // namespace Assisi::Core
