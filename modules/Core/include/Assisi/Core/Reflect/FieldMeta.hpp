/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/FieldMeta.hpp
/// @brief Descriptor for a single reflected component field.

#include <cstddef>
#include <string>

namespace Assisi::Core::Reflect
{

enum class FieldType
{
    Float,
    Double,
    Int,
    Int32,
    UInt32,
    Bool,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    Mat4,
    EntityRef,       ///< ECS::Entity serialized as a stable serial index via SceneSerializer.
    AssetPath,       ///< Core::AssetPath — a fixed-capacity virtual asset path, serialized as a string.
    AssetPathVector, ///< std::vector<Core::AssetPath> — serialized as a JSON array of strings.
    Unknown,
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
};

} // namespace Assisi::Core::Reflect