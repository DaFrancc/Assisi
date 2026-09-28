/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestInternedString.cpp
/// @brief One index per text, the empty name at index 0, and interning from many
/// threads at once agreeing on every index.

#include <doctest/doctest.h>

#include <ostream>

#include <cstdint>
#include <latch>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include <Assisi/Core/InternedString.hpp>

using Assisi::Core::InternedString;

namespace
{

/// Threads interning at once, and texts each interns. Enough that the texts
/// cross a chunk boundary while the threads race.
constexpr std::uint32_t kThreads       = 8;
constexpr std::uint32_t kTextsPerThread = 6000;

std::string ThreadedText(std::uint32_t index)
{
    return "interned-race-" + std::to_string(index);
}

} // namespace

TEST_CASE("InternedString: the same text from two constructions is the same integer")
{
    const InternedString first{"node.title"};
    const InternedString second{std::string{"node."} + "title"};
    CHECK(first.Index() == second.Index());
    CHECK(first == second);
    CHECK(first.View() == "node.title");
}

TEST_CASE("InternedString: different texts are different integers")
{
    const InternedString a{"style.a"};
    const InternedString b{"style.b"};
    CHECK(a.Index() != b.Index());
    CHECK_FALSE(a == b);
}

TEST_CASE("InternedString: the default is the empty name, index 0")
{
    const InternedString empty;
    CHECK(empty.Index() == 0);
    CHECK(empty.Empty());
    CHECK(empty.View().empty());
    CHECK(InternedString{""} == empty);
    CHECK_FALSE(InternedString{"x"}.Empty());
}

TEST_CASE("InternedString: a text is stored as given, not as the caller's buffer")
{
    std::string buffer = "mutable-source";
    const InternedString name{buffer};
    buffer[0] = 'X';
    CHECK(name.View() == "mutable-source");
}

TEST_CASE("InternedString: threads interning the same texts at once agree on every index")
{
    // Every thread interns the same texts in the same order, released together,
    // so the first sighting of each is contested. A table that did not look up
    // and append under one lock would hand one text two indices.
    std::vector<std::vector<std::uint32_t>> seen(kThreads, std::vector<std::uint32_t>(kTextsPerThread));
    std::latch start{kThreads};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (std::uint32_t thread = 0; thread < kThreads; ++thread)
    {
        threads.emplace_back(
            [thread, &seen, &start]
            {
                start.arrive_and_wait();
                for (std::uint32_t text = 0; text < kTextsPerThread; ++text)
                {
                    const InternedString name{ThreadedText(text)};
                    // Read back at once, from this thread, while others append.
                    if (name.View() != ThreadedText(text))
                    {
                        seen[thread][text] = 0;
                        continue;
                    }
                    seen[thread][text] = name.Index();
                }
            });
    }
    for (std::thread &thread : threads)
    {
        thread.join();
    }

    std::unordered_set<std::uint32_t> distinct;
    for (std::uint32_t text = 0; text < kTextsPerThread; ++text)
    {
        const std::uint32_t index = seen[0][text];
        REQUIRE(index != 0);
        for (std::uint32_t thread = 1; thread < kThreads; ++thread)
        {
            REQUIRE(seen[thread][text] == index);
        }
        distinct.insert(index);
        CHECK(InternedString{ThreadedText(text)}.Index() == index);
    }
    CHECK(distinct.size() == kTextsPerThread);
}
