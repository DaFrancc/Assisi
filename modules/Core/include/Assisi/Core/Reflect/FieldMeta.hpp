/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/FieldMeta.hpp
/// @brief Descriptor for a single reflected component field.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Assisi::Core::Reflect
{

enum class FieldType
{
    Float,
    Double,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Bool,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    Mat4,
    Enum,            ///< An AENUM-annotated `enum class`; stored as its underlying integer (see enumSize).
    String,          ///< Core::ShortString — a fixed-capacity inline string, serialized as a string.
    EntityRef,       ///< ECS::Entity serialized as a stable serial index via SceneSerializer.
    AssetPath,       ///< Core::AssetPath — a fixed-capacity virtual asset path, serialized as a string.
    AssetPathVector, ///< std::vector<Core::AssetPath> — serialized as a JSON array of strings.
    AssetId,         ///< Core::AssetId — a stable GUID reference, serialized as { guid, path-hint }.
    AssetIdVector,   ///< std::vector<Core::AssetId> — serialized as a JSON array of { guid, path-hint }.
    Unknown,
};

/// @brief One enumerator of a reflected `enum class` (FieldType::Enum).
///
/// The value is the enumerator's integer value; an editor presents `name` and
/// writes `value` back. Serialization uses the value, so reordering enumerators
/// is safe but changing an existing enumerator's value migrates saved data.
struct EnumConstant
{
    std::string  name;
    std::int64_t value = 0;
};

/// @brief What an editor does to a "radio" listener field when its source enum
/// is not at one of the field's active values (see FieldMeta::radioSource).
enum class RadioBehavior
{
    None,   ///< Not a radio listener (the default).
    Grey,   ///< Disable (grey out) the field while inactive.
    Vanish, ///< Hide the field entirely while inactive.
};

struct FieldMeta
{
    std::string name;
    FieldType   type      = FieldType::Unknown;
    std::size_t offset    = 0;
    bool        transient = false; ///< If true, excluded from serialization.

    // Editor hints from AFIELD(min=..., max=...): inclusive bounds an editor
    // must clamp numeric edits to (e.g. a light radius that must not go
    // negative). Hints only — serialization does not enforce them.
    bool  hasMin   = false; ///< True when AFIELD supplied min=...
    bool  hasMax   = false; ///< True when AFIELD supplied max=...
    float minValue = 0.f;   ///< Inclusive lower bound; meaningful when hasMin.
    float maxValue = 0.f;   ///< Inclusive upper bound; meaningful when hasMax.

    // Populated only for FieldType::Enum: the enumerators to offer in an editor,
    // in declaration order. Empty for every other field type.
    std::vector<EnumConstant> enumConstants;

    // The enum's underlying storage, so an editor reads/writes the field at its
    // true width instead of assuming a 4-byte int (which would corrupt neighbours
    // for an 8/16-bit enum). enumSize is the byte width (1/2/4/8); 0 marks a
    // non-enum field. enumSigned selects sign-extension when reading.
    std::uint8_t enumSize   = 0;     ///< Underlying byte width; 0 = not an enum.
    bool         enumSigned = false; ///< Underlying type is signed (sign-extend on read).

    // Radio: declarative editor visibility driven by a sibling enum's value. A
    // field annotated AFIELD(radioListen = { source = enumField, value = ...,
    // behavior = grey|vanish }) is "active" only while that sibling enum equals
    // one of radioValues; otherwise the editor greys or hides it per
    // radioBehavior. A broadcaster enum (AFIELD(radioBroadcast)) carries no radio
    // metadata here — it is an ordinary enum field that listeners reference by
    // name. Sources may themselves be listeners: when a source is inactive its
    // listeners hide unconditionally, so the editor resolves visibility by
    // walking radioSource up the chain (reflectgen rejects cycles). radioSource is
    // empty for every non-listener field.
    std::string               radioSource;                          ///< Sibling enum field this field's visibility follows ("" = not a listener).
    std::vector<std::int64_t> radioValues;                          ///< Enum values at which this field is active; meaningful when radioSource set.
    RadioBehavior             radioBehavior = RadioBehavior::None;   ///< Editor treatment while inactive.
};

} // namespace Assisi::Core::Reflect