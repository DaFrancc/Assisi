/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestReflectedStructs.hpp
/// @brief A document shaped like a cooked screen, reflected only for the tests:
/// a root holding a pool, a list of nodes, each holding a style of lengths.
///
/// Probe and Grown differ by one AFIELD in their style and nothing else, which
/// is exactly the edit a field added to Style or ScreenNode makes. That the new
/// field round-trips and moves the layout, with no other line anywhere naming
/// it, is what the tests that use these check.

#include <Assisi/Core/InternedString.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Core/StringPool.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace Assisi::Mondrian::Testing
{

AENUM()
enum class ProbeUnit : uint8_t
{
    Pixels,
    Share,
    Count
};

ASTRUCT()
struct ProbeLength
{
    AFIELD() float value = 0.f;
    AFIELD() ProbeUnit unit = ProbeUnit::Pixels;

    [[nodiscard]] friend bool operator==(const ProbeLength &, const ProbeLength &) = default;
};

ASTRUCT()
struct ProbeStyle
{
    AFIELD() std::array<ProbeLength, 2> sizes {};
    AFIELD() ProbeLength gap;
    AFIELD() std::array<bool, 2> scrolls {};

    [[nodiscard]] friend bool operator==(const ProbeStyle &, const ProbeStyle &) = default;
};

/// ProbeStyle with one more field, added the only way a field is added.
ASTRUCT()
struct GrownStyle
{
    AFIELD() std::array<ProbeLength, 2> sizes {};
    AFIELD() ProbeLength gap;
    AFIELD() std::array<bool, 2> scrolls {};
    AFIELD() float opacity = 1.f;

    [[nodiscard]] friend bool operator==(const GrownStyle &, const GrownStyle &) = default;
};

ASTRUCT()
struct ProbeNode
{
    AFIELD() ProbeStyle style;
    AFIELD() Core::PooledString text;
    AFIELD() Core::InternedString name;
    AFIELD() uint32_t parent = 0;

    [[nodiscard]] friend bool operator==(const ProbeNode &, const ProbeNode &) = default;
};

ASTRUCT()
struct GrownNode
{
    AFIELD() GrownStyle style;
    AFIELD() Core::PooledString text;
    AFIELD() Core::InternedString name;
    AFIELD() uint32_t parent = 0;

    [[nodiscard]] friend bool operator==(const GrownNode &, const GrownNode &) = default;
};

ASTRUCT()
struct ProbeDocument
{
    AFIELD() Core::StringPool pool;
    AFIELD() std::vector<ProbeNode> nodes;
    AFIELD() std::vector<Core::InternedString> systems;

    [[nodiscard]] friend bool operator==(const ProbeDocument &, const ProbeDocument &) = default;
};

ASTRUCT()
struct GrownDocument
{
    AFIELD() Core::StringPool pool;
    AFIELD() std::vector<GrownNode> nodes;
    AFIELD() std::vector<Core::InternedString> systems;

    [[nodiscard]] friend bool operator==(const GrownDocument &, const GrownDocument &) = default;
};

} // namespace Assisi::Mondrian::Testing
