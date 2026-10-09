/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestSuggest.cpp
/// @brief A misspelt name suggests the closest real one, counting a swap of two
/// letters as one mistake, and suggests nothing when nothing is close.

#include <doctest/doctest.h>

#include <Assisi/Sigil/Compile/Suggest.hpp>

#include <array>

using Assisi::Sigil::Compile::ClosestName;
using Assisi::Sigil::Compile::DidYouMean;

TEST_CASE("Suggest: the closest name, a swap counting as one edit")
{
    const std::array<std::string_view, 3> names{"walk", "run", "idle"};
    CHECK(ClosestName(names, "wlak") == std::optional<std::string_view>{"walk"});
    CHECK(ClosestName(names, "rn") == std::optional<std::string_view>{"run"});
    CHECK_FALSE(ClosestName(names, "zzzzzz").has_value());
    // A name is never suggested for itself.
    CHECK_FALSE(ClosestName(names, "walk").has_value());
    CHECK(DidYouMean(names, "idel") == " — did you mean \"idle\"?");
    CHECK(DidYouMean(names, "zzzzzz").empty());
}
