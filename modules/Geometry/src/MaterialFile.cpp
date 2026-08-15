/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Geometry/MaterialFile.hpp>

#include <cstdint>
#include <typeindex>

#include <nlohmann/json.hpp>

#include <Assisi/Core/Reflect/AssetTypeRegistry.hpp>

namespace Assisi::Geometry
{
namespace
{
constexpr int32_t kMaterialFileVersion = 1;

/// The registered asset meta for MaterialData, or nullptr if the generated
/// reflection object was not linked into this binary.
const Core::Reflect::AssetTypeMeta *MaterialMeta()
{
    return Core::Reflect::AssetTypeRegistry::Instance().Find(std::type_index(typeid(MaterialData)));
}
} // namespace

std::string_view ToString(MaterialFileError error) noexcept
{
    switch (error)
    {
    case MaterialFileError::NotRegistered:
        return "MaterialData reflection not registered";
    case MaterialFileError::ParseFailed:
        return "invalid JSON";
    case MaterialFileError::WrongType:
        return "wrong asset type";
    }
    return "unknown error";
}

std::expected<std::string, MaterialFileError> SerializeMaterial(const MaterialData &material)
{
    const Core::Reflect::AssetTypeMeta *meta = MaterialMeta();
    if (meta == nullptr)
    {
        return std::unexpected(MaterialFileError::NotRegistered);
    }

    // Envelope first so "version"/"type" read at the top of the file; the
    // reflected field payload is merged in flat beneath them.
    nlohmann::json document;
    document["version"] = kMaterialFileVersion;
    document["type"] = meta->name;

    const nlohmann::json fields = meta->serialize(&material);
    for (const auto &[key, value] : fields.items())
    {
        document[key] = value;
    }

    return document.dump(2);
}

std::expected<MaterialData, MaterialFileError> DeserializeMaterial(std::string_view jsonText)
{
    const Core::Reflect::AssetTypeMeta *meta = MaterialMeta();
    if (meta == nullptr)
    {
        return std::unexpected(MaterialFileError::NotRegistered);
    }

    const nlohmann::json document = nlohmann::json::parse(jsonText, nullptr, /*allow_exceptions=*/ false);
    if (document.is_discarded() || !document.is_object())
    {
        return std::unexpected(MaterialFileError::ParseFailed);
    }

    if (document.value("type", std::string{}) != meta->name)
    {
        return std::unexpected(MaterialFileError::WrongType);
    }

    // Start from defaults; deserialize applies only the keys present, so a file
    // written by an older engine (missing newer fields) still loads cleanly.
    // The envelope keys "version"/"type" are ignored (no field is named them).
    MaterialData material;
    meta->deserialize(document, &material);
    return material;
}

} /* namespace Assisi::Geometry */
