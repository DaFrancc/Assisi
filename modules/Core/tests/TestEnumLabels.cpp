/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestEnumLabels.cpp
/// @brief The registry of names a program gives an enum's unnamed values.
///
/// The registry is process-wide, so each case names an enum of its own.

#include <doctest/doctest.h>

#include <Assisi/Core/Reflect/EnumLabels.hpp>

#include <span>

using namespace Assisi::Core::Reflect;

TEST_CASE("An enum nobody labelled has no labels")
{
    CHECK(EnumLabelsOf("Test::NeverLabelled").empty());
}

TEST_CASE("Labels come back in order of value, whatever order they were given in")
{
    RegisterEnumLabel("Test::Ordered", 9, "Late");
    RegisterEnumLabel("Test::Ordered", 6, "Early");

    const std::span<const EnumConstant> labels = EnumLabelsOf("Test::Ordered");
    REQUIRE(labels.size() == 2);
    CHECK(labels[0].value == 6);
    CHECK(labels[0].name == "Early");
    CHECK(labels[1].value == 9);
    CHECK(labels[1].name == "Late");
}

TEST_CASE("A second label for one value is refused, and the first is kept")
{
    RegisterEnumLabel("Test::Twice", 7, "First");
    RegisterEnumLabel("Test::Twice", 7, "Second");

    const std::span<const EnumConstant> labels = EnumLabelsOf("Test::Twice");
    REQUIRE(labels.size() == 1);
    CHECK(labels[0].name == "First");
}

TEST_CASE("Labels for one enum are not another's")
{
    RegisterEnumLabel("Test::Mine", 6, "Mine");
    CHECK(EnumLabelsOf("Test::Yours").empty());
}
