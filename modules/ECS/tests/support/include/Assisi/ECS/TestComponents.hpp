/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestComponents.hpp
/// @brief Reflected components used only by the ECS test suite.
///
/// Scene indexes its pools by Core::Reflect::ComponentId, so every component a
/// Scene stores must be registered with the reflection system (ACOMP). These
/// stand in for the ad-hoc structs the tests used before that requirement,
/// exercising the real reflected-component path. They are compiled only into
/// the test binary (see modules/ECS/tests/CMakeLists.txt).

#include <Assisi/Prelude.hpp>

namespace Assisi::ECS
{

ACOMP()
struct Position
{
    AFIELD() float x = 0.0f;
};

ACOMP()
struct Velocity
{
    AFIELD() float x = 0.0f;
};

ACOMP()
struct Tag
{
};

/// @brief Opts into change detection (ACOMP(tracked)) so the change-tick tests
/// exercise the tracked storage/access path; Position/Velocity above stay
/// untracked, giving a control that must always report tick 0.
ACOMP(tracked)
struct Tracked
{
    AFIELD() int32_t value = 0;
};

} // namespace Assisi::ECS
