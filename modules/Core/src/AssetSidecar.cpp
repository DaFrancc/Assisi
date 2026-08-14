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
    const nlohmann::json document = nlohmann::json::parse(jsonText, nullptr, /*allow_exceptions=*/ false);
    if (document.is_discarded() || !document.is_object())
    {
        return std::unexpected(AssetSidecarError::ParseFailed);
    }

    // Field access is by is_*()-guarded find() rather than value(): nlohmann's
    // value(key, default) only substitutes the default when the key is *absent*
    // and throws json::type_error when it is present with the wrong type. Since
    // this returns std::expected (and the parse used allow_exceptions=false), a
    // wrong-typed field must map to an error, never escape as an exception.
    const auto typeIt = document.find("type");
    if (typeIt == document.end() || !typeIt->is_string() || typeIt->get<std::string>() != kAssetSidecarType)
    {
        return std::unexpected(AssetSidecarError::WrongType);
    }

    const auto guidIt = document.find("guid");
    if (guidIt == document.end() || !guidIt->is_string())
    {
        return std::unexpected(AssetSidecarError::MissingGuid);
    }
    const std::optional<AssetId> guid = AssetId::Parse(guidIt->get<std::string>());
    if (!guid.has_value())
    {
        return std::unexpected(AssetSidecarError::MissingGuid);
    }

    AssetSidecar sidecar = AssetSidecar::Leaf(*guid);

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
            const auto materialIt = entry.find("material");
            if (materialIt == entry.end() || !materialIt->is_string())
            {
                continue;
            }
            const std::optional<AssetId> material = AssetId::Parse(materialIt->get<std::string>());
            if (!material.has_value())
            {
                continue;
            }
            // Slot must be a non-negative integer under the slot cap: read as u64
            // so a value near UINT32_MAX can't wrap, and reject anything absurd (a
            // negative literal is is_number_unsigned()==false, so it is skipped).
            std::uint32_t slot = 0;
            if (const auto slotIt = entry.find("slot"); slotIt != entry.end())
            {
                if (!slotIt->is_number_unsigned())
                {
                    continue;
                }
                const std::uint64_t rawSlot = slotIt->get<std::uint64_t>();
                if (rawSlot >= kMaxMaterialSlots)
                {
                    continue;
                }
                slot = static_cast<std::uint32_t>(rawSlot);
            }
            sidecar.subAssets.push_back(AssetSubAsset{.slot = slot, .material = *material});
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
