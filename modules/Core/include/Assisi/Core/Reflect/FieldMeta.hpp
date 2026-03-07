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
    Unknown,
};

struct FieldMeta
{
    std::string name;
    FieldType   type      = FieldType::Unknown;
    std::size_t offset    = 0;
    bool        transient = false; ///< If true, excluded from serialization.
};

} // namespace Assisi::Core::Reflect