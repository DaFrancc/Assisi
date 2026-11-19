/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CookComponents.hpp
/// @brief A reflected component with a field held back from the network, used
/// only by the Runtime test suite to cook levels that hold one.

#include <Assisi/Prelude.hpp>

#include <cstdint>

namespace Assisi::Runtime::Tests
{

/// @brief Host bookkeeping beside a value every machine sees: the shape
/// AFIELD(norep) exists for. A level saves both, so a cooked level must too.
ACOMP(replicable)
struct CookedTally
{
    /// Sent to every machine.
    AFIELD() int32_t shown = 0;
    /// Saved with the level, never sent.
    AFIELD(norep) int32_t hostOnly = 0;
};

} // namespace Assisi::Runtime::Tests
