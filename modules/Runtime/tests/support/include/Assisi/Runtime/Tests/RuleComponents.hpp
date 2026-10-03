/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file RuleComponents.hpp
/// @brief Reflected components with requires/excludes rules, used only by the
/// Runtime test suite to load files that exercise them.

#include <Assisi/Prelude.hpp>

#include <cstdint>

namespace Assisi::Runtime::Tests
{

/// @brief Required by LoadNeeds. Its value is what a load must not reset.
ACOMP()
struct LoadState
{
    AFIELD() int32_t value = 0;
};

/// @brief Sorts before LoadState, so a file lists it first and its default
/// LoadState arrives before the saved one.
ACOMP(requires = {LoadState})
struct LoadNeeds
{
};

/// @brief Cannot share an entity with LoadState.
ACOMP(excludes = {LoadState})
struct LoadShuns
{
};

/// @brief Requires LoadShuns, so it cannot join an entity holding LoadState
/// even though it excludes nothing itself.
ACOMP(requires = {LoadShuns})
struct LoadNeedsShuns
{
};

} // namespace Assisi::Runtime::Tests
