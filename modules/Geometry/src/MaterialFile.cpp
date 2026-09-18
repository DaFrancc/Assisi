/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Geometry/MaterialFile.hpp>

#include <Assisi/Core/Reflect/AssetDocument.hpp>

namespace Assisi::Geometry
{
namespace
{
/// @brief The .amat spelling of an envelope failure.
///
/// A material has no field the reader can reject on its own terms — every type
/// the schema uses either reads or is absent — so BadField arrives only from a
/// value of the wrong JSON type, which is the same thing to a caller as a
/// document that will not parse.
MaterialFileError FromDocumentError(Core::Reflect::AssetDocumentError error)
{
    switch (error)
    {
    case Core::Reflect::AssetDocumentError::NotRegistered:
        return MaterialFileError::NotRegistered;
    case Core::Reflect::AssetDocumentError::WrongType:
        return MaterialFileError::WrongType;
    case Core::Reflect::AssetDocumentError::ParseFailed:
    case Core::Reflect::AssetDocumentError::BadField:
        return MaterialFileError::ParseFailed;
    }
    return MaterialFileError::ParseFailed;
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
    const std::expected<std::string, Core::Reflect::AssetDocumentError> text =
        Core::Reflect::SerializeAssetDocument(material);
    if (!text)
    {
        return std::unexpected(FromDocumentError(text.error()));
    }
    return *text;
}

std::expected<MaterialData, MaterialFileError> DeserializeMaterial(std::string_view jsonText)
{
    // Start from defaults; the document applies only the keys it names, so a
    // file written by an older engine still loads and the fields it predates
    // keep the value this build ships.
    MaterialData material;
    const std::expected<void, Core::Reflect::AssetDocumentError> applied =
        Core::Reflect::ApplyAssetDocument(jsonText, material);
    if (!applied)
    {
        return std::unexpected(FromDocumentError(applied.error()));
    }
    return material;
}

} /* namespace Assisi::Geometry */
