/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file StringPool.hpp
/// @brief Many strings belonging to one owner, end to end in one buffer.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace Assisi::Core
{

/// @brief The most bytes one pool may hold.
///
/// Offsets are 32-bit, which is the hard ceiling; this is lower so a corrupt or
/// hostile byte count read off the wire cannot ask for gigabytes before the
/// reader notices the bytes are not there.
inline constexpr std::size_t kMaxPoolBytes = std::size_t{64} * 1024 * 1024;

/// @brief One string in a StringPool: where it starts and how long it is.
///
/// Means nothing without the pool it came from. A struct holding PooledStrings
/// holds exactly one StringPool beside them, which reflectgen enforces, so which
/// pool a handle belongs to is never in doubt.
struct PooledString
{
    std::uint32_t offset = 0;
    std::uint32_t length = 0;

    bool operator==(const PooledString &other) const = default;
};

static_assert(std::is_trivially_copyable_v<PooledString>, "a PooledString is copied and sent as its two integers");

/// @brief One buffer holding an owner's strings end to end.
///
/// No per-string allocation, no reserved capacity and no length cap per string:
/// a table of PooledStrings walks one buffer. Strings are only ever appended, so
/// a handle stays valid for as long as the pool does; replacing a string means
/// adding its new text and repointing the handle, which leaves the old bytes in
/// place.
class StringPool
{
public:
    /// @brief Append @p text and return its handle.
    ///
    /// Past kMaxPoolBytes this asserts, and in a build without asserts logs an
    /// error and returns the empty handle.
    PooledString Add(std::string_view text);

    /// @brief The text @p handle names, or empty when it reaches outside the pool
    /// — a handle decoded from untrusted bytes is checked here, not trusted.
    [[nodiscard]] std::string_view View(PooledString handle) const;

    /// @brief Every byte, in the order the strings were added.
    [[nodiscard]] std::string_view Bytes() const { return _bytes; }

    /// @brief Replace every byte with @p bytes, as a loader does.
    /// @return false, leaving the pool unchanged, when @p bytes is over kMaxPoolBytes.
    bool Assign(std::string_view bytes);

    void Clear() { _bytes.clear(); }

    [[nodiscard]] std::size_t Size() const { return _bytes.size(); }

    bool operator==(const StringPool &other) const = default;

private:
    std::string _bytes;
};

} // namespace Assisi::Core
