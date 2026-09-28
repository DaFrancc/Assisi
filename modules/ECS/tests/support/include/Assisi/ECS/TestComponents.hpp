/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestComponents.hpp
/// @brief Reflected components used only by the ECS test suite.
///
/// Scene indexes its pools by Core::Reflect::ComponentId, so every component a
/// Scene stores must be registered with the reflection system (ACOMP). These
/// exercise the real reflected-component path and are compiled only into the
/// test binary (see modules/ECS/tests/CMakeLists.txt).

#include <Assisi/Prelude.hpp>

#include <Assisi/Core/DisplayedString.hpp>
#include <Assisi/Core/InternedString.hpp>
#include <Assisi/Core/StringPool.hpp>

#include <vector>

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

/// @brief ACOMP(transient): registered only for a stable ComponentId, with no
/// serialization hooks at all. The control for tests that check a consumer
/// gates on ComponentMeta::serializable rather than probing each hook.
ACOMP(transient)
struct TransientTag
{
};

/// @brief One of each reflected string type, so the generated JSON and the
/// binary codec are exercised through the code reflectgen actually writes.
ACOMP()
struct Captions
{
    AFIELD() Core::StringPool pool;
    AFIELD() std::vector<Core::PooledString> rows;
    AFIELD() Core::DisplayedString label;
    AFIELD() Core::InternedString style;
    AFIELD() Core::PooledString title;
};

} // namespace Assisi::ECS
