/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Layout.hpp
/// @brief Where each value lives in the block an expression runs against.
///
/// A block is one word per slot: the file's params in the order declared, then
/// the vocabulary's functions, whose values the engine always provides, then
/// the lets, which a let pass fills each frame before anything reads them.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Sigil
{

enum class SlotType : uint8_t
{
    Float,
    Int, ///< Ints and enum values.
    Bool, ///< Bools and triggers, as 0 or 1.
    None, ///< A param of a vocabulary type, such as a clip: no expression reads it.
    Count_,
};

enum class SlotKind : uint8_t
{
    Param,
    Implied, ///< A vocabulary function's value, written by the engine.
    Let,
    Count_,
};

struct Slot
{
    std::string name;
    SlotType type = SlotType::None;
    SlotKind kind = SlotKind::Param;
};

struct Layout
{
    std::vector<Slot> slots;
    uint32_t paramCount = 0;
    uint32_t impliedCount = 0;
    uint32_t letCount = 0;

    [[nodiscard]] uint32_t ParamSlot(uint32_t param) const
    {
        return param;
    }

    [[nodiscard]] uint32_t ImpliedSlot(uint32_t implied) const
    {
        return paramCount + implied;
    }

    [[nodiscard]] uint32_t LetSlot(uint32_t let) const
    {
        return paramCount + impliedCount + let;
    }
};

/// @brief The slot of the param, function or let named @p name; names in a
///        file never collide, so there is at most one.
[[nodiscard]] std::optional<uint32_t> FindSlot(const Layout &layout, std::string_view name);

} // namespace Assisi::Sigil
