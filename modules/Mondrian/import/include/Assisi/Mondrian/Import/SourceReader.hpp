/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SourceReader.hpp
/// @brief How a compile reaches source files other than the one it was handed.

#include <expected>
#include <functional>
#include <string>
#include <string_view>

namespace Assisi::Mondrian::Import
{

/// @brief Reads the text of the file at an asset path, or says why it cannot.
///
/// How a compile reaches the template libraries a screen imports, the string
/// tables it names and the UI settings. Taken rather than reached for, so a
/// test can serve files from memory.
using SourceReader = std::function<std::expected<std::string, std::string>(std::string_view vpath)>;

} // namespace Assisi::Mondrian::Import
