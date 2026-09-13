/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestSystemSearch.cpp
/// @brief What the Systems panel's search field offers, and in what order.
///
/// The ranking is the part of that panel with a right answer. A field that put
/// `PreShadow` above `Shadow` for the query `shadow` would be wrong in a way an
/// author feels on every keystroke and cannot work around.

#include <doctest/doctest.h>

#include <Assisi/Editor/SystemSearch.hpp>

#include <string_view>
#include <vector>

using Assisi::Editor::RankSystemMatches;
using Assisi::Editor::SystemSearchHit;

namespace
{

std::vector<std::string_view> Names(const std::vector<SystemSearchHit> &hits)
{
    std::vector<std::string_view> names;
    names.reserve(hits.size());
    for (const SystemSearchHit &hit : hits)
        names.push_back(hit.name);
    return names;
}

} // namespace

TEST_CASE("An empty query offers nothing")
{
    const std::vector<std::string_view> candidates{"Bounce", "Shadow"};
    CHECK(RankSystemMatches(candidates, "").empty());
}

TEST_CASE("Matching is case-insensitive in both directions")
{
    const std::vector<std::string_view> candidates{"Bounce"};
    CHECK(Names(RankSystemMatches(candidates, "BOU")) == std::vector<std::string_view>{"Bounce"});
    CHECK(Names(RankSystemMatches(candidates, "nce")) == std::vector<std::string_view>{"Bounce"});
}

TEST_CASE("A match nearer the start of the name outranks one further in")
{
    const std::vector<std::string_view> candidates{"PreShadowPass", "Shadow"};
    CHECK(Names(RankSystemMatches(candidates, "shadow")) ==
          std::vector<std::string_view>{"Shadow", "PreShadowPass"});
}

TEST_CASE("Equal match positions break by name length, shortest first")
{
    const std::vector<std::string_view> candidates{"BounceDamping", "Bounce"};
    CHECK(Names(RankSystemMatches(candidates, "bounce")) ==
          std::vector<std::string_view>{"Bounce", "BounceDamping"});
}

TEST_CASE("Candidates that tie on both keys keep the order they arrived in")
{
    // Same match position, same length — so the only thing left to order them is
    // the registration order the panel's list below is also drawn in.
    const std::vector<std::string_view> candidates{"Zeta", "Beta"};
    CHECK(Names(RankSystemMatches(candidates, "eta")) == std::vector<std::string_view>{"Zeta", "Beta"});
}

TEST_CASE("A name with no occurrence of the query is left out")
{
    const std::vector<std::string_view> candidates{"Bounce", "Shadow"};
    CHECK(Names(RankSystemMatches(candidates, "physics")).empty());
}

TEST_CASE("The reported position is where the match sits in the name")
{
    const std::vector<std::string_view> candidates{"PreShadowPass"};
    const std::vector<SystemSearchHit> hits = RankSystemMatches(candidates, "shadow");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].position == 3);
}
