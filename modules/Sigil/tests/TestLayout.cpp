/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestLayout.cpp
/// @brief A param is found by its name and type, and a miss says why.

#include <Assisi/Sigil/Layout.hpp>

#include <doctest/doctest.h>

using namespace Assisi::Sigil;

namespace
{

/// Two params, a function's value and a let, as a file declaring
/// `param speed: float; param grounded: bool;` with one let lays them out.
Layout Sample()
{
    Layout layout;
    layout.slots = {{.name = "speed", .type = SlotType::Float, .kind = SlotKind::Param},
        {.name = "grounded", .type = SlotType::Bool, .kind = SlotKind::Param},
        {.name = "progress", .type = SlotType::Float, .kind = SlotKind::Implied},
        {.name = "moving", .type = SlotType::Bool, .kind = SlotKind::Let}};
    layout.paramCount = 2;
    layout.impliedCount = 1;
    layout.letCount = 1;
    return layout;
}

} // namespace

TEST_CASE("Layout: a param is found by its name and type")
{
    const Layout layout = Sample();
    CHECK(FindParam(layout, "speed", SlotType::Float) == 0u);
    CHECK(FindParam(layout, "grounded", SlotType::Bool) == 1u);
}

TEST_CASE("Layout: a param not found says whether the name is missing, not a param, or of another type")
{
    const Layout layout = Sample();
    CHECK(FindParam(layout, "sped", SlotType::Float).error() == ParamMiss::Missing);
    CHECK(FindParam(layout, "progress", SlotType::Float).error() == ParamMiss::NotParam);
    CHECK(FindParam(layout, "moving", SlotType::Bool).error() == ParamMiss::NotParam);
    CHECK(FindParam(layout, "grounded", SlotType::Float).error() == ParamMiss::OtherType);
}
