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
#include <Assisi/ECS/Entity.hpp>

#include <array>
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

/// @brief Requires Position, so adding it adds a default Position.
ACOMP(requires = {Position})
struct Needs
{
    AFIELD() int32_t value = 0;
};

/// @brief Requires Needs, so adding it reaches Position through Needs.
ACOMP(requires = {Needs})
struct NeedsNeeds
{
};

/// @brief Cannot share an entity with Position, declared from this side only.
ACOMP(excludes = {Position})
struct Shuns
{
};

/// @brief Requires Shuns, so an entity with Position can take neither.
ACOMP(requires = {Shuns})
struct NeedsShuns
{
};

/// @brief A transient requirement: it has no serialization hooks, but adding
/// NeedsGhost must still add it.
ACOMP(transient)
struct Ghost
{
};

ACOMP(requires = {Ghost})
struct NeedsGhost
{
};

/// @brief Excludes the hierarchy's Parent, as a body whose pose physics owns does.
ACOMP(excludes = {Parent})
struct Rooted
{
};

/// @brief A reflected entity reference, so the generated JSON's route through
/// the entity-reference codec is exercised by the code reflectgen writes.
ACOMP()
struct Link
{
    AFIELD() ECS::Entity target = NullEntity;
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

AENUM()
enum class Tone : uint8_t
{
    Plain,
    Loud = 4,
    Count
};

/// @brief A value struct holding a pooled string it leaves to its holder's pool,
/// an enum, and an array.
ASTRUCT()
struct Line
{
    AFIELD() Core::PooledString text;
    AFIELD() std::array<uint8_t, 2> marks {};
    AFIELD() Tone tone = Tone::Plain;
    AFIELD(transient) float scratch = 0.f;
};

/// @brief Structs held every way a component can hold one: alone, in a list,
/// in an array, and in a struct that holds them in turn.
ASTRUCT()
struct Verse
{
    AFIELD() Line first;
    AFIELD() std::vector<Line> rest;
};

ACOMP()
struct Poem
{
    AFIELD() Core::StringPool pool;
    AFIELD() Line title;
    AFIELD() std::array<Line, 2> epigraphs {};
    AFIELD() std::vector<Verse> verses;
};

} // namespace Assisi::ECS
