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
    Count,
};

/// Fills every bit, the width at which All() cannot use a shift.
enum class Wide : std::uint8_t
{
    First,
    Last = 31,
    Count,
};

enum Unscoped
{
    UnscopedA,
    Count,
};

enum class NoCount
{
    A,
};

enum class TooWide : std::uint8_t
{
    First,
    Last = 32,
    Count,
};

} // namespace

// Reflection reads a Bitmask field as FieldType::UInt32 at the field's offset,
// so it has to be exactly its uint32_t.
static_assert(sizeof(Bitmask<Fruit>) == sizeof(std::uint32_t));
static_assert(std::is_standard_layout_v<Bitmask<Fruit>>);
static_assert(std::is_trivially_copyable_v<Bitmask<Fruit>>);
static_assert(offsetof(Bitmask<Fruit>, bits) == 0);

static_assert(Assisi::Core::BitmaskEnum<Fruit>);
static_assert(Assisi::Core::BitmaskEnum<Wide>);
static_assert(!Assisi::Core::BitmaskEnum<NoCount>);
static_assert(!Assisi::Core::BitmaskEnum<TooWide>);
static_assert(!Assisi::Core::BitmaskEnum<std::uint32_t>);
static_assert(!Assisi::Core::BitmaskEnum<Unscoped>);

TEST_CASE("Bitmask: All is exactly the enumerators before Count")
{
    CHECK(Bitmask<Fruit>::All().bits == 0b111u);
    CHECK(Bitmask<Wide>::All().bits == 0xFFFF'FFFFu);
}

TEST_CASE("Bitmask: an enumerator's bit is its value")
{
    CHECK(Bitmask<Fruit>::Of(Fruit::Apple).bits == 0b001u);
    CHECK(Bitmask<Fruit>::Of(Fruit::Plum).bits == 0b100u);
}

TEST_CASE("Bitmask: With and Without change only the named bit")
{
    const Bitmask<Fruit> some = Bitmask<Fruit>::Of(Fruit::Apple).With(Fruit::Plum);
    CHECK(some.Has(Fruit::Apple));
    CHECK_FALSE(some.Has(Fruit::Pear));
    CHECK(some.Has(Fruit::Plum));

    const Bitmask<Fruit> fewer = Bitmask<Fruit>::All().Without(Fruit::Pear);
    CHECK(fewer == some);
    CHECK(fewer.Without(Fruit::Pear) == fewer);
}

TEST_CASE("Bitmask: the default is empty")
{
    const Bitmask<Fruit> none{};
    CHECK(none.bits == 0u);
    CHECK_FALSE(none.Has(Fruit::Apple));
}
