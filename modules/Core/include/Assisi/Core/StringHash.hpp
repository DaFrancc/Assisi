/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file StringHash.hpp
/// @brief Transparent string hashing for allocation-free heterogeneous lookup.

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace Assisi::Core
{

/// @brief Transparent hash enabling heterogeneous lookup on string-keyed maps.
///
/// Pair with `std::equal_to<>` on an `unordered_map`/`unordered_set` so
/// `find`/`contains` accept a `std::string_view` (or `const char*`) without
/// allocating a temporary `std::string` per call:
/// @code
/// std::unordered_map<std::string, V, Core::TransparentStringHash, std::equal_to<>>
/// @endcode
struct TransparentStringHash
{
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view sv) const noexcept
    {
        return std::hash<std::string_view>{}(sv);
    }
    [[nodiscard]] std::size_t operator()(const std::string &s) const noexcept
    {
        return std::hash<std::string_view>{}(s);
    }
    [[nodiscard]] std::size_t operator()(const char *s) const noexcept
    {
        return std::hash<std::string_view>{}(s);
    }
};

} // namespace Assisi::Core
