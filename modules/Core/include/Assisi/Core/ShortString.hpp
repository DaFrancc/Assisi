/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShortString.hpp
/// @brief A small fixed-capacity inline string for names and labels.

#include <cstddef>

#include <Assisi/Core/TrivialString.hpp>

namespace Assisi::Core
{

/// @brief Maximum number of bytes a ShortString can hold (32).
inline constexpr std::size_t kShortStringMax = 32;

/// @brief A short, human-readable inline string — e.g. an entity Name.
///
/// A TrivialString at a small capacity: heap-free, trivially copyable, and safe
/// to store inline in a component. reflectgen keys on the spelled type name
/// `Assisi::Core::ShortString`, so it serializes as a plain string and the editor
/// renders it as a text box (FieldType::String).
using ShortString = TrivialString<kShortStringMax>;

} // namespace Assisi::Core
