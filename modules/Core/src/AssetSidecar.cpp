/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/AssetSidecar.hpp>

#include <Assisi/Core/ContentHash.hpp>

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

    // Composite manifest: emitted only when present, so a leaf asset's sidecar
    // stays a plain `{version,type,guid}` (unchanged from S1).
    if (!sidecar.subAssets.empty())
    {
        nlohmann::json subAssets = nlohmann::json::array();
        for (const AssetSubAsset &entry : sidecar.subAssets)
        {
            nlohmann::json object;
            object["slot"]     = entry.slot;
            object["material"] = entry.material.ToString();
            subAssets.push_back(std::move(object));
        }
        document["subAssets"] = std::move(subAssets);
    }

    // Source hash: a fixed-width hex string (a 64-bit value doesn't fit a JSON
    // number without precision loss). Emitted only when present.
    if (sidecar.sourceHash.has_value())
    {
        document["sourceHash"] = ToHex64(*sidecar.sourceHash);
    }

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

    AssetSidecar sidecar{.guid = *guid};

    // Composite manifest (optional). A malformed entry is skipped rather than
    // failing the whole sidecar: identity (the guid) already validated, and the
    // manifest is advisory relationship data that the reconcile pass can rebuild.
    if (const auto found = document.find("subAssets"); found != document.end() && found->is_array())
    {
        for (const nlohmann::json &entry : *found)
        {
            if (!entry.is_object())
            {
                continue;
            }
            const std::optional<AssetId> material = AssetId::Parse(entry.value("material", std::string{}));
            if (!material.has_value())
            {
                continue;
            }
            sidecar.subAssets.push_back(
                AssetSubAsset{.slot = entry.value("slot", std::uint32_t{0}), .material = *material});
        }
    }

    // Source hash (optional): a malformed value is dropped, not fatal — a missing
    // hash simply reads as "never stamped", which the reconciler treats as stale.
    if (const auto found = document.find("sourceHash"); found != document.end() && found->is_string())
    {
        sidecar.sourceHash = FromHex64(found->get<std::string>());
    }

    return sidecar;
}

} // namespace Assisi::Core
