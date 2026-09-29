/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/StructMeta.hpp
/// @brief A reflected value struct (ASTRUCT): a field table any other
///        reflected type can hold inline, alone or in a container.
///
/// A struct's table is generated once, in the generated file of the header that
/// declares it. Every other file reaches it through StructTraits, whose
/// specialization ASSISI_REFLECTED_STRUCT declares: reflectgen writes one at the
/// top of each generated file that names the struct, and hand-written code that
/// wants a struct's table — a cooked format that is one struct, say — writes one
/// itself. A program that uses the table without linking the generated file
/// fails to link, rather than finding no table at run time.

#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <Assisi/Core/Reflect/FieldMeta.hpp>

namespace Assisi::Core::Reflect
{

/// @brief One ASTRUCT's name and fields, in declaration order.
///
/// Offsets are relative to the struct itself, so a codec walks a nested struct
/// by adding the field's offset to the address of the struct holding it.
struct StructSpec
{
    std::string name;
    std::vector<FieldMeta> fields;
};

/// @brief Whether @p T is an ASTRUCT, and its table and JSON if it is.
///
/// The primary template says no. ASSISI_REFLECTED_STRUCT says yes.
template <typename T> struct StructTraits
{
    static constexpr bool reflected = false;
};

} // namespace Assisi::Core::Reflect

/// @brief Declares that @p Type is an ASTRUCT whose table its header's
/// generated file defines. At global scope, before anything in the file uses
/// the type's StructTraits; every declaration of one type must be identical,
/// which is why this is a macro.
#define ASSISI_REFLECTED_STRUCT(Type)                                                                                  \
    template <> struct Assisi::Core::Reflect::StructTraits<Type>                                                       \
    {                                                                                                                  \
        static constexpr bool reflected = true;                                                                        \
        static const ::Assisi::Core::Reflect::StructSpec *Spec();                                                      \
        static ::nlohmann::json ToJson(const void *ptr);                                                               \
        static bool FromJson(const ::nlohmann::json &j, void *out_ptr);                                                \
    }
