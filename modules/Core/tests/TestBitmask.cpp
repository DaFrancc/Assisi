/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBitmask.cpp
/// @brief Core::Bitmask: set operations, and the layout reflection relies on.

#include <doctest/doctest.h>

#include <Assisi/Core/Bitmask.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

using Assisi::Core::Bitmask;

namespace
{

enum class Fruit : std::uint8_t
{
    Apple,
    Pear,
    Plum,
    Count_,
};

/// Fills every bit, the width at which All() cannot use a shift.
enum class Wide : std::uint8_t
{
    First,
    Last = 31,
    Count_,
};

enum Unscoped
{
    UnscopedA,
    Count_,
};

enum class NoCount
{
    A,
};

enum class TooWide : std::uint8_t
{
    First,
    Last = 32,
    Count_,
};

/// Fills a byte: the width at which an 8-bit All() cannot use a shift.
enum class Byte : std::uint8_t
{
    First,
    Last = 7,
    Count_,
};

/// Fills 64 bits.
enum class Widest : std::uint8_t
{
    First,
    Last = 63,
    Count_,
};

using FruitMask = Bitmask<Fruit, std::uint32_t>;
using SmallFruitMask = Bitmask<Fruit, std::uint8_t>;

} // namespace

// Reflection reads a Bitmask field as the FieldType of its storage at the
// field's offset, so it has to be exactly that integer.
static_assert(sizeof(FruitMask) == sizeof(std::uint32_t));
static_assert(sizeof(SmallFruitMask) == sizeof(std::uint8_t));
static_assert(sizeof(Bitmask<Fruit, std::uint16_t>) == sizeof(std::uint16_t));
static_assert(sizeof(Bitmask<Fruit, std::uint64_t>) == sizeof(std::uint64_t));
static_assert(std::is_standard_layout_v<SmallFruitMask>);
static_assert(std::is_trivially_copyable_v<SmallFruitMask>);
static_assert(offsetof(SmallFruitMask, bits) == 0);

static_assert(Assisi::Core::BitmaskEnum<Fruit, std::uint8_t>);
static_assert(Assisi::Core::BitmaskEnum<Wide, std::uint32_t>);
static_assert(!Assisi::Core::BitmaskEnum<Wide, std::uint16_t>);
static_assert(Assisi::Core::BitmaskEnum<TooWide, std::uint64_t>);
static_assert(!Assisi::Core::BitmaskEnum<TooWide, std::uint32_t>);
static_assert(!Assisi::Core::BitmaskEnum<NoCount, std::uint32_t>);
static_assert(!Assisi::Core::BitmaskEnum<std::uint32_t, std::uint32_t>);
static_assert(!Assisi::Core::BitmaskEnum<Unscoped, std::uint32_t>);

// Only the four fixed-width unsigned integers have a FieldType to be read as.
static_assert(!Assisi::Core::BitmaskEnum<Fruit, bool>);
static_assert(!Assisi::Core::BitmaskEnum<Fruit, std::int32_t>);
static_assert(!Assisi::Core::BitmaskEnum<Fruit, char32_t>);

TEST_CASE("Bitmask: All is exactly the enumerators before Count_")
{
    CHECK(FruitMask::All().bits == 0b111u);
    CHECK(SmallFruitMask::All().bits == 0b111u);
    CHECK(Bitmask<Wide, std::uint32_t>::All().bits == 0xFFFF'FFFFu);
    CHECK(Bitmask<Byte, std::uint8_t>::All().bits == 0xFFu);
    CHECK(Bitmask<Widest, std::uint64_t>::All().bits == 0xFFFF'FFFF'FFFF'FFFFu);
}

TEST_CASE("Bitmask: an enumerator's bit is its value")
{
    CHECK(FruitMask::Of(Fruit::Apple).bits == 0b001u);
    CHECK(FruitMask::Of(Fruit::Plum).bits == 0b100u);
    CHECK(Bitmask<Byte, std::uint8_t>::Of(Byte::Last).bits == 0x80u);
    CHECK(Bitmask<Widest, std::uint64_t>::Of(Widest::Last).bits == 0x8000'0000'0000'0000u);
}

TEST_CASE("Bitmask: With and Without change only the named bit")
{
    const FruitMask some = FruitMask::Of(Fruit::Apple).With(Fruit::Plum);
    CHECK(some.Has(Fruit::Apple));
    CHECK_FALSE(some.Has(Fruit::Pear));
    CHECK(some.Has(Fruit::Plum));

    const FruitMask fewer = FruitMask::All().Without(Fruit::Pear);
    CHECK(fewer == some);
    CHECK(fewer.Without(Fruit::Pear) == fewer);

    // A narrow mask is promoted to int by every operation; the result must
    // still be the byte it started as.
    const SmallFruitMask small = SmallFruitMask::All().Without(Fruit::Apple);
    CHECK(small.bits == 0b110u);
    CHECK_FALSE(small.Has(Fruit::Apple));
    CHECK(small.With(Fruit::Apple) == SmallFruitMask::All());
}

TEST_CASE("Bitmask: the default is empty")
{
    const FruitMask none{};
    CHECK(none.bits == 0u);
    CHECK_FALSE(none.Has(Fruit::Apple));
}
