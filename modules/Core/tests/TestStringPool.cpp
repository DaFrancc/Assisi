/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestStringPool.cpp
/// @brief A pool holds its strings end to end in one buffer, and a handle that
/// reaches outside it reads as empty rather than out of bounds.

#include <doctest/doctest.h>

#include <ostream>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <Assisi/Core/StringPool.hpp>

using Assisi::Core::PooledString;
using Assisi::Core::StringPool;

namespace
{

/// Strings in the table the one-buffer case walks.
constexpr std::uint32_t kTableRows = 500;

} // namespace

TEST_CASE("StringPool: each string reads back as it was added")
{
    StringPool pool;
    const PooledString title = pool.Add("Paused");
    const PooledString empty = pool.Add("");
    const PooledString quit  = pool.Add("Quit to menu");

    CHECK(pool.View(title) == "Paused");
    CHECK(pool.View(empty).empty());
    CHECK(pool.View(quit) == "Quit to menu");
    CHECK(pool.Bytes() == "PausedQuit to menu");
}

TEST_CASE("StringPool: a table of PooledStrings walks one buffer")
{
    StringPool pool;
    std::vector<PooledString> table;
    for (std::uint32_t row = 0; row < kTableRows; ++row)
    {
        table.push_back(pool.Add("row " + std::to_string(row) + " has a label longer than an inline buffer"));
    }

    const std::string_view bytes = pool.Bytes();
    const char *const begin      = bytes.data();
    const char *const end        = bytes.data() + bytes.size();
    const char *expected         = begin;
    for (std::uint32_t row = 0; row < kTableRows; ++row)
    {
        const std::string_view text = pool.View(table[row]);
        // Each string sits in the one buffer, directly after the one before it.
        REQUIRE(text.data() == expected);
        REQUIRE(text.data() + text.size() <= end);
        CHECK(text == "row " + std::to_string(row) + " has a label longer than an inline buffer");
        expected = text.data() + text.size();
    }
    CHECK(expected == end);
}

TEST_CASE("StringPool: a handle outside the pool reads as empty")
{
    StringPool pool;
    (void)pool.Add("abc");
    CHECK(pool.View(PooledString{.offset = 1, .length = 2}) == "bc");
    CHECK(pool.View(PooledString{.offset = 4, .length = 0}).empty());
    CHECK(pool.View(PooledString{.offset = 2, .length = 5}).empty());
    // offset + length wraps a 32-bit sum back inside the pool.
    CHECK(pool.View(PooledString{.offset = 1, .length = 0xFFFFFFFFu}).empty());
}

TEST_CASE("StringPool: Assign replaces the bytes and refuses a pool over the limit")
{
    StringPool pool;
    (void)pool.Add("old");
    CHECK(pool.Assign("newbytes"));
    CHECK(pool.Bytes() == "newbytes");

    const std::string tooLarge(Assisi::Core::kMaxPoolBytes + 1, 'x');
    CHECK_FALSE(pool.Assign(tooLarge));
    CHECK(pool.Bytes() == "newbytes");
}
