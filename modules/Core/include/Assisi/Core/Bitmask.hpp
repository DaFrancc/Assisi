/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Bitmask.hpp
/// @brief A set of one enum's enumerators, one bit each.
///
/// Not std::bitset: that is indexed by a bare size_t, so nothing ties a set to
/// the enum it holds, and its size and word layout are left to the standard
/// library. A Bitmask is exactly its unsigned integer — reflection reads and
/// writes it at the field's offset as the FieldType of that width, and the wire
/// carries that integer — while the enum parameter keeps one enum's set from
/// being mixed with another's. A file stores the names of the enumerators it
/// holds instead; see Reflect/BitmaskJson.hpp.
///
/// The width is written at the declaration, `Bitmask<E, std::uint8_t>`, and
/// defaults to 32 bits. It is never inferred from the enum: an enumerator added
/// past the width fails the build where the mask is declared, rather than
/// silently growing every struct that holds one.
///
/// An enumerator's value is its bit position, so renumbering one changes what
/// every mask already in memory or on the wire means.

#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace Assisi::Core
{

/// @brief An integer a Bitmask may store its bits in: one of the four
/// fixed-width unsigned types, which reflection has a FieldType for each of.
template <typename Bits>
concept BitmaskStorage = std::same_as<Bits, std::uint8_t> || std::same_as<Bits, std::uint16_t> ||
                         std::same_as<Bits, std::uint32_t> || std::same_as<Bits, std::uint64_t>;

/// How many bits a Bitmask stored in @p Bits has.
template <BitmaskStorage Bits> inline constexpr std::uint32_t kBitmaskWidth = std::numeric_limits<Bits>::digits;

/// @brief A scoped enum with a trailing `Count_` that fits in a Bitmask stored
/// in @p Bits.
///
/// `Count_` is what All() is built from, so the full set widens with the enum
/// instead of needing a matching edit.
template <typename E, typename Bits = std::uint32_t>
concept BitmaskEnum = std::is_scoped_enum_v<E> && BitmaskStorage<Bits> && requires { E::Count_; } &&
static_cast<std::uint32_t>(E::Count_) <= kBitmaskWidth<Bits>;

/// @brief A set of @p E's enumerators; bit N is the enumerator whose value is N.
///
/// An aggregate with one public member, so it is standard-layout with `bits` at
/// offset zero and copies as the integer it is.
template <typename E, typename Bits = std::uint32_t>
requires BitmaskEnum<E, Bits>
struct Bitmask
{
    Bits bits = 0;

    /// @brief The set holding only @p value.
    [[nodiscard]] static constexpr Bitmask Of(E value) { return Bitmask{BitOf(value)}; }

    /// @brief Every enumerator before `Count_`.
    [[nodiscard]] static constexpr Bitmask All()
    {
        constexpr std::uint32_t count = static_cast<std::uint32_t>(E::Count_);
        // Shifting by the full width is undefined, so a full-width enum is spelled out.
        if constexpr (count == kBitmaskWidth<Bits>)
        {
            return Bitmask{std::numeric_limits<Bits>::max()};
        }
        else
        {
            return Bitmask{static_cast<Bits>((Bits{1} << count) - 1u)};
        }
    }

    [[nodiscard]] constexpr bool Has(E value) const { return (bits & BitOf(value)) != Bits{0}; }

    [[nodiscard]] constexpr Bitmask With(E value) const { return Bitmask{static_cast<Bits>(bits | BitOf(value))}; }

    [[nodiscard]] constexpr Bitmask Without(E value) const
    {
        return Bitmask{static_cast<Bits>(bits & ~BitOf(value))};
    }

    bool operator==(const Bitmask &) const = default;

private:
    // Cast back because a narrow Bits is promoted to int by the shift.
    static constexpr Bits BitOf(E value) { return static_cast<Bits>(Bits{1} << static_cast<std::uint32_t>(value)); }
};

} // namespace Assisi::Core
