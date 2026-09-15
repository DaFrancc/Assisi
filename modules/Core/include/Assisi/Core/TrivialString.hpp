/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TrivialString.hpp
/// @brief Fixed-capacity, length-prefixed, trivially-copyable inline string.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace Assisi::Core
{

/// @brief Default character capacity for a TrivialString.
inline constexpr std::size_t kDefaultTrivialStringCapacity = 127;

/// @brief A heap-free, length-prefixed string stored inline in a fixed buffer.
///
/// No null terminator — the character count is stored explicitly — so a
/// TrivialString never allocates and stays trivially copyable (cheap to hold in
/// an ECS component and memcpy-safe). The buffer is private and Assign() is its
/// only writer, so it cannot be overflowed.
///
/// Content is treated as opaque bytes, so UTF-8 is stored losslessly by default;
/// Capacity and Size() are byte counts, not code points (same convention as
/// std::string). Comparison and std::hash are defined over View(), so they never
/// depend on the unused tail bytes. Assign() truncates on overflow, backing up
/// off a split multi-byte UTF-8 sequence so View() stays valid UTF-8 for valid
/// input.
///
/// @tparam Capacity Maximum number of bytes (excludes the length prefix).
template <std::size_t Capacity = kDefaultTrivialStringCapacity> class TrivialString
{
    static_assert(Capacity <= 0xFFFFu,
                  "TrivialString capacity must fit in its uint16_t length prefix");

public:
    static constexpr std::size_t kCapacity = Capacity;

    TrivialString() = default;
    explicit TrivialString(std::string_view text) { Assign(text); }

    /// @brief Replaces the contents with @p text, truncating to Capacity bytes.
    /// @return false if @p text was longer than Capacity and had to be truncated
    ///         (so a caller can log/reject); true otherwise.
    bool Assign(std::string_view text)
    {
        const bool truncated = text.size() > Capacity;
        std::size_t count     = truncated ? Capacity : text.size();

        // On truncation, don't slice through a multi-byte UTF-8 code point: back
        // up off any trailing continuation bytes (0b10xxxxxx) so View() stays
        // valid UTF-8. text[count] is in range here because count < text.size().
        if (truncated)
            while (count > 0 && (static_cast<std::uint8_t>(text[count]) & 0xC0) == 0x80)
                --count;

        std::copy_n(text.data(), count, _data.data());
        _length = static_cast<std::uint16_t>(count);
        return !truncated;
    }

    [[nodiscard]] std::string_view View() const { return {_data.data(), _length}; }
    [[nodiscard]] bool             Empty() const { return _length == 0; }
    [[nodiscard]] std::size_t      Size() const { return _length; }

    /// @brief Returns a heap-allocated std::string copy of the contents.
    [[nodiscard]] std::string ToString() const { return std::string(View()); }

    /// @brief Copies the contents into @p buffer as a null-terminated C string,
    /// writing at most @p bufferSize - 1 characters plus the terminator. The
    /// TrivialString itself stays terminator-free; the terminator lives only in
    /// the caller's buffer.
    /// @return the number of characters written, excluding the terminator.
    std::size_t ToCStr(char *buffer, std::size_t bufferSize) const
    {
        if (bufferSize == 0)
            return 0;
        const std::size_t count = std::min(static_cast<std::size_t>(_length), bufferSize - 1);
        std::copy_n(_data.data(), count, buffer);
        buffer[count] = '\0';
        return count;
    }

    bool operator==(const TrivialString &other) const { return View() == other.View(); }

    /// @brief Lexicographic order over View(), agreeing with operator== and never
    /// reading the unused tail bytes.
    ///
    /// A map keyed by one of these is serialized in sorted key order, so the
    /// encoding is the same on every run whatever order the keys were inserted in.
    /// Without an order the codec would have to fall back on the container's own
    /// iteration, which for an unordered_map follows memory layout.
    auto operator<=>(const TrivialString &other) const { return View() <=> other.View(); }

private:
    std::array<char, Capacity> _data{};
    std::uint16_t _length = 0;
};

/// @brief Maximum number of bytes an EntityName can hold (64).
///
/// Separate from ShortString's 32 so a name can be long without widening every
/// short label. Runtime/Naming.hpp sizes its refusals and its uniquing suffix
/// from here.
inline constexpr std::size_t kEntityNameMax = 64;

/// @brief The name of an entity: heap-free, fixed-capacity, inline.
///
/// A spelling, not a distinct type: reflectgen accepts this and a bare
/// `TrivialString<64>` alike, and they decode identically. Use this one where
/// the value is an entity's name and the bare form for any other text wanting
/// the wider capacity, so the declaration says which it means. Serializes as a
/// plain string; the editor draws it as a text box.
///
/// Thirty-two and sixty-four are the only reflectable capacities — the binary
/// codec reads into the buffer by capacity and has a field type for each. A
/// field declaring any other capacity fails the build by name.
using EntityName = TrivialString<kEntityNameMax>;

} // namespace Assisi::Core

template <std::size_t Capacity> struct std::hash<Assisi::Core::TrivialString<Capacity>>
{
    std::size_t operator()(const Assisi::Core::TrivialString<Capacity> &s) const noexcept
    {
        return std::hash<std::string_view>{}(s.View());
    }
};
