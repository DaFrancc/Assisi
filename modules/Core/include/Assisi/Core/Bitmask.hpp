/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Bitmask.hpp
/// @brief A set of one enum's enumerators, one bit each.
///
/// Not std::bitset: that is indexed by a bare size_t, so nothing ties a set to
/// the enum it holds, and its size and word layout are left to the standard
/// library. A Bitmask is exactly its uint32_t — reflection reads and writes it
/// at the field's offset as a FieldType::UInt32, and the on-disk form is that
/// integer — while the enum parameter keeps one enum's set from being mixed with
/// another's.
///
/// An enumerator's value is its bit position, so an enum used here is append
/// only: renumbering one re-aims every mask already saved.

#include <cstdint>
#include <type_traits>

namespace Assisi::Core
{

/// Bitmask stores its bits in a uint32_t.
inline constexpr std::uint32_t kBitmaskBits = 32;

/// @brief A scoped enum with a trailing `Count` that fits in a Bitmask.
///
/// `Count` is what All() is built from, so the full set widens with the enum
/// instead of needing a matching edit.
template <typename E>
concept BitmaskEnum = std::is_scoped_enum_v<E> && requires { E::Count; } &&
                      static_cast<std::uint32_t>(E::Count) <= kBitmaskBits;

/// @brief A set of @p E's enumerators; bit N is the enumerator whose value is N.
///
/// An aggregate with one public member, so it is standard-layout with `bits` at
/// offset zero and copies as the integer it is.
template <BitmaskEnum E> struct Bitmask
{
    std::uint32_t bits = 0;

    /// @brief The set holding only @p value.
    [[nodiscard]] static constexpr Bitmask Of(E value) { return Bitmask{BitOf(value)}; }

    /// @brief Every enumerator before `Count`.
    [[nodiscard]] static constexpr Bitmask All()
    {
        constexpr std::uint32_t count = static_cast<std::uint32_t>(E::Count);
        // Shifting a uint32_t by 32 is undefined, so a full-width enum is spelled out.
        if constexpr (count == kBitmaskBits)
        {
            return Bitmask{~0u};
        }
        else
        {
            return Bitmask{(1u << count) - 1u};
        }
    }

    [[nodiscard]] constexpr bool Has(E value) const { return (bits & BitOf(value)) != 0u; }

    [[nodiscard]] constexpr Bitmask With(E value) const { return Bitmask{bits | BitOf(value)}; }

    [[nodiscard]] constexpr Bitmask Without(E value) const { return Bitmask{bits & ~BitOf(value)}; }

    bool operator==(const Bitmask &) const = default;

private:
    static constexpr std::uint32_t BitOf(E value) { return 1u << static_cast<std::uint32_t>(value); }
};

} // namespace Assisi::Core
