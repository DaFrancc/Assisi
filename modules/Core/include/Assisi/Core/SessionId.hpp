/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SessionId.hpp
/// @brief Which network session a browse entry or a hosted game is.

#include <Assisi/Core/ContentHash.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace Assisi::Core
{

/// @brief A network session's identity: 128 random bits drawn when it is hosted.
///
/// A browse list is keyed by it, so a repeat search updates a known session in
/// place however its name or player count changed. Random rather than counted,
/// because two hosts on one network count from the same place. In Core rather
/// than NetSync because a game reads it through `ctx.net` in every build.
///
/// All zero is no session.
struct SessionId
{
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    [[nodiscard]] constexpr bool IsValid() const { return high != 0 || low != 0; }

    /// @brief 32 hex digits, for a log line or a debug label.
    [[nodiscard]] std::string ToString() const { return ToHex64(high) + ToHex64(low); }

    friend constexpr bool operator==(const SessionId &, const SessionId &) = default;
    friend constexpr std::strong_ordering operator<=>(const SessionId &, const SessionId &) = default;
};

} // namespace Assisi::Core

template <> struct std::hash<Assisi::Core::SessionId>
{
    [[nodiscard]] std::size_t operator()(const Assisi::Core::SessionId &id) const noexcept
    {
        // Already random, so either half alone is a fine hash; both together
        // cost one XOR.
        return std::hash<std::uint64_t>{}(id.high ^ id.low);
    }
};
