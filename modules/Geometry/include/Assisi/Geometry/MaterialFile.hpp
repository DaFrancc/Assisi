/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MaterialFile.hpp
/// @brief .amat (Assisi material) text serialization for MaterialData.
///
/// The field payload is produced by the reflection system (MaterialData is an
/// AASSET, so adding a field flows through automatically); this layer only adds
/// and validates the `{ "version", "type", <fields> }` envelope. Pure
/// text-in/text-out — no filesystem — so callers choose how the bytes are read
/// or written (editor via AssetSystem, tests via a string).

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <Assisi/Geometry/MaterialData.hpp>

namespace Assisi::Geometry
{

/// @brief Why a .amat (de)serialize failed.
enum class MaterialFileError : std::uint8_t
{
    NotRegistered, ///< MaterialData's reflection isn't linked (generated object missing).
    ParseFailed,   ///< The text is not valid JSON.
    WrongType,     ///< The envelope's "type" is not MaterialData.
};

std::string_view ToString(MaterialFileError error) noexcept;

/// @brief Serialize a material to .amat JSON text (pretty-printed).
std::expected<std::string, MaterialFileError> SerializeMaterial(const MaterialData &material);

/// @brief Parse .amat JSON text into a MaterialData. Fields absent from the
///        text keep their default value (forward-compatible as fields are
///        added). Validates the envelope "type".
std::expected<MaterialData, MaterialFileError> DeserializeMaterial(std::string_view jsonText);

} /* namespace Assisi::Geometry */
