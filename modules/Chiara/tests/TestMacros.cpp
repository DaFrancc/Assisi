/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file TestMacros.cpp
/// @brief The macros in a *default* build.
///
/// Deliberately unguarded: this file compiles in both configurations and is the
/// only proof that instrumentation left in the tree still type-checks when
/// Chiara is off. Without it the no-op path is never compiled by anyone until
/// someone builds with `-c` and discovers a year of bit-rot.

#include <Assisi/Chiara/Profile.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <string>

namespace
{

std::int32_t g_evaluations = 0;

/// `maybe_unused` because being unreferenced is the thing the second case below
/// proves: in a Chiara-disabled build ASSISI_PROFILE_COUNTER never evaluates its
/// argument, so this is parsed, type-checked and never called. Clang notices
/// (-Wunneeded-internal-declaration) and is right — it is describing the
/// behaviour under test, not a mistake.
[[nodiscard]] [[maybe_unused]] double CountedValue()
{
    ++g_evaluations;
    return 1.0;
}

} // namespace

TEST_CASE("Every profile macro compiles regardless of the build configuration")
{
    ASSISI_PROFILE_FRAME();

    ASSISI_PROFILE_SCOPE("test-scope");
    ASSISI_PROFILE_FUNCTION();
    ASSISI_PROFILE_ARG_STR("key", "value");
    ASSISI_PROFILE_ARG_U64("count", 7u);
    ASSISI_PROFILE_COUNTER("test/value", 1.5);
    ASSISI_PROFILE_FLOW_BEGIN("test-flow", 1u);
    ASSISI_PROFILE_FLOW_END("test-flow", 1u);

    CHECK(true); // Reaching here is the assertion; the compile is the test.
}

TEST_CASE("A disabled build does not evaluate macro arguments")
{
    // The Assert.hpp contract, inherited: arguments stay parsed and type-checked
    // but are never evaluated, so instrumentation is free rather than merely
    // cheap — and a variable used only by a profile macro still counts as used.
    g_evaluations = 0;
    ASSISI_PROFILE_COUNTER("test/counted", CountedValue());

#if defined(ASSISI_CHIARA_ENABLED)
    CHECK(g_evaluations == 1);
#else
    CHECK(g_evaluations == 0);
#endif
}

TEST_CASE("Scoped names may be dynamic through interning")
{
    // Exercises the documented path for a name that is not a literal. In a
    // default build InternString is an inline stub, so this still compiles.
    const std::string dynamicName = std::string("dynamic-") + "scope";
    const char * const interned    = Assisi::Chiara::InternString(dynamicName);
    ASSISI_PROFILE_SCOPE(interned);

    CHECK(interned != nullptr);
}
