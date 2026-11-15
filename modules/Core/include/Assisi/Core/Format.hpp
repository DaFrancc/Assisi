/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Format.hpp
/// @brief std::format, with its formatting engine compiled once.
///
/// std::format instantiates the whole formatting engine in every file that
/// calls it, which adds about three seconds to an optimised compile of that
/// file. Format checks its format string at compile time exactly as std::format
/// does, then hands the arguments, type-erased, to VFormat, which is compiled
/// once in Core. Only each argument type's formatter is instantiated at the
/// call site.

#include <format>
#include <string>
#include <string_view>

namespace Assisi::Core
{

/// @brief @p format filled in with @p args, as std::vformat does. The format
/// string is checked only at run time; call Format, which checks it while
/// compiling, unless the format string is not known until then.
/// @throws std::format_error if @p format does not match @p args.
[[nodiscard]] std::string VFormat(std::string_view format, std::format_args args);

/// @brief @p format filled in with @p args, as std::format does.
template <typename... Args> [[nodiscard]] std::string Format(std::format_string<Args...> format, Args &&... args)
{
    return VFormat(format.get(), std::make_format_args(args...));
}

} // namespace Assisi::Core
