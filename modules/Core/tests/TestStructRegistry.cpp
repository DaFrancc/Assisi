/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Core/Reflect/StructRegistry.hpp>

using namespace Assisi::Core::Reflect;

namespace
{

const StructSpec *FirstSpec()
{
    static const StructSpec spec{.name = "Shared", .fields = {}};
    return &spec;
}

const StructSpec *SecondSpec()
{
    static const StructSpec spec{.name = "Shared", .fields = {}};
    return &spec;
}

} // namespace

TEST_CASE("StructRegistry: a struct is found by the name it was registered under")
{
    StructRegistry registry;
    registry.Register("Shared", &FirstSpec);

    CHECK(registry.Find("Shared") == FirstSpec());
    CHECK(registry.Find("Missing") == nullptr);
}

TEST_CASE("StructRegistry: a second struct of one name is refused, and the first stays")
{
    StructRegistry registry;
    registry.Register("Shared", &FirstSpec);
    registry.Register("Shared", &SecondSpec);

    CHECK(registry.Find("Shared") == FirstSpec());
}
