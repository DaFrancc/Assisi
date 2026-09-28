/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file InternedString.hpp
/// @brief A name the engine compares and a player never reads, held as an index
///        into one process-wide table of texts.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace Assisi::Core
{

/// @brief Most distinct texts the intern table holds in one run.
///
/// Four million names is far past anything a game names; the bound exists so the
/// table's chunk index is a fixed array that never moves under a reader.
inline constexpr std::uint32_t kMaxInternedStrings = 1u << 22;

/// @brief A name stored once for the whole process and compared as an integer.
///
/// Two InternedStrings made from the same text hold the same index, so equality
/// and hashing never touch the text. The index is only meaningful within one run:
/// texts are numbered in the order they are first interned, which differs from
/// run to run, so anything that leaves the process — a file, a packet — carries
/// View() and interns it again on the other side.
///
/// There is deliberately no ordering. An order by index would differ between runs
/// and make anything sorted by it encode differently each time; an order by text
/// would hide a string comparison behind what reads as an integer one.
///
/// The table is append-only. Interning takes a lock; View() does not, and is safe
/// from any thread, because an entry never moves or changes once it is published.
class InternedString
{
public:
    /// @brief The empty name, which is index 0 in every run.
    InternedString() = default;

    /// @brief The name for @p text, adding it to the table the first time it is
    /// seen.
    ///
    /// When the table is full this asserts, and in a build without asserts logs
    /// an error and gives the empty name, so a runaway interner degrades names
    /// rather than memory.
    explicit InternedString(std::string_view text);

    /// @brief The text. Valid for the life of the process.
    [[nodiscard]] std::string_view View() const;

    /// @brief The table index: equal for equal texts, within this run only.
    [[nodiscard]] std::uint32_t Index() const { return _index; }

    [[nodiscard]] bool Empty() const { return _index == 0; }

    bool operator==(const InternedString &other) const = default;

private:
    std::uint32_t _index = 0;
};

/// @brief How many distinct texts the table holds, the empty name included.
[[nodiscard]] std::uint32_t InternedStringCount();

} // namespace Assisi::Core

template <> struct std::hash<Assisi::Core::InternedString>
{
    std::size_t operator()(const Assisi::Core::InternedString &name) const noexcept
    {
        return std::hash<std::uint32_t>{}(name.Index());
    }
};
