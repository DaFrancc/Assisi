/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file BytecodeTesting.hpp
/// @brief Robot files compiled down to bytecode and run, so a test can write
///        an expression and check the value the game would get.

#include "SigilTesting.hpp"

#include <Assisi/Sigil/Evaluate.hpp>
#include <Assisi/Sigil/Verify.hpp>
#include <Assisi/Sigil/Compile/Lower.hpp>

#include <doctest/doctest.h>

#include <initializer_list>
#include <utility>

namespace Assisi::Sigil::Testing
{

/// A file's lets as code, and the block they run against.
struct Lowered
{
    std::vector<Word> code;
    std::vector<uint32_t> lets;
    Layout layout;
};

using Values = std::initializer_list<std::pair<std::string_view, Word>>;

/// @p body, after `use robot;`, compiled and its lets lowered, each checked by Verify.
inline Lowered LowerRobot(std::string_view body)
{
    const std::expected<Program, Diagnostics> program = CompileRobot("use robot;\n" + std::string{body});
    REQUIRE_MESSAGE(program.has_value(), Dump(Errors(program)));
    Lowered lowered;
    lowered.layout = MakeLayout(*program, Robot());
    std::expected<std::vector<uint32_t>, std::string> lets = LowerLets(*program, lowered.layout, lowered.code);
    REQUIRE_MESSAGE(lets.has_value(), lets.error());
    lowered.lets = std::move(*lets);
    for (const uint32_t entry : lowered.lets)
    {
        const std::expected<void, std::string> verified =
            Verify(lowered.code, entry, static_cast<uint32_t>(lowered.layout.slots.size()));
        REQUIRE_MESSAGE(verified.has_value(), verified.error());
    }
    return lowered;
}

/// A block for @p layout with @p values written to the slots they name, and
/// zero elsewhere.
inline std::vector<Word> BlockOf(const Layout &layout, Values values)
{
    std::vector<Word> block(layout.slots.size(), 0);
    for (const std::pair<std::string_view, Word> &value : values)
    {
        const std::optional<uint32_t> slot = FindSlot(layout, value.first);
        REQUIRE_MESSAGE(slot.has_value(), value.first);
        block[*slot] = value.second;
    }
    return block;
}

/// The value of `let r = expression;` after @p declarations, with @p values
/// written to the block first.
inline Word Run(std::string_view declarations, std::string_view expression, Values values)
{
    const Lowered lowered = LowerRobot(std::format("{}let r = {};\n", declarations, expression));
    std::vector<Word> block = BlockOf(lowered.layout, values);
    EvaluateLets(lowered.code, lowered.lets, lowered.layout, block);
    return block[lowered.layout.LetSlot(lowered.layout.letCount - 1)];
}

/// The code `let r = expression;` after @p declarations lowers to.
inline std::string CodeOf(std::string_view declarations, std::string_view expression)
{
    const Lowered lowered = LowerRobot(std::format("{}let r = {};\n", declarations, expression));
    return Disassemble(lowered.code, lowered.lets.back());
}

} // namespace Assisi::Sigil::Testing
