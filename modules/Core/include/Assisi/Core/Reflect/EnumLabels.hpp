/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file EnumLabels.hpp
/// @brief Names a program gives to values a reflected enum leaves unnamed.
///
/// Some enums reserve slots for the program to use, with no enumerator for any
/// of them: a game's collision channels are values of CollisionChannel that only
/// the game can name. An editor showing such a field asks here for those names,
/// keyed by the enum's fully qualified name as FieldMeta::enumType spells it.
///
/// Registered once, at static initialization, and read afterwards: nothing here
/// is guarded for registration racing a read.

#include <Assisi/Core/Reflect/FieldMeta.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace Assisi::Core::Reflect
{

/// @brief Names @p value of the enum @p enumType as @p label.
///
/// A second label for a value that already has one is refused with a warning,
/// and the first is kept: two names for one value would leave an editor showing
/// whichever came first in an order nobody chose.
void RegisterEnumLabel(std::string_view enumType, std::int64_t value, std::string_view label);

/// @brief Every label registered for @p enumType, in order of value. Empty when
/// nothing named any of its values.
[[nodiscard]] std::span<const EnumConstant> EnumLabelsOf(std::string_view enumType);

} // namespace Assisi::Core::Reflect
