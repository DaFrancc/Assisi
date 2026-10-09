/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestIntegration.cpp
/// @brief The factory examples sglc ships, end to end: the good one compiles
///        without a warning, every value in it becomes code that passes
///        Verify, and a frame of it decides what a game would; the broken one
///        reports each of its mistakes once, on its own line, in order.

#include "BytecodeTesting.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace Assisi::Sigil;
using namespace Assisi::Sigil::Testing;

namespace
{

const std::filesystem::path kExamples{ASSISI_SGLC_EXAMPLES};

std::expected<std::string, std::string> ReadExample(std::string_view path)
{
    std::ifstream file{kExamples / path};
    if (!file)
    {
        return std::unexpected("no such file");
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

std::expected<Program, Diagnostics> CompileExample(std::string_view path)
{
    const std::expected<std::string, std::string> source = ReadExample(path);
    REQUIRE_MESSAGE(source.has_value(), path);
    const std::array<Vocabulary, 1> vocabularies{Robot()};
    return CompileSource(*source, path, vocabularies, ReadExample);
}

/// Every expression the game would run in @p block and the blocks inside it:
/// transition conditions, and clause values that are numbers or bools.
void CollectExpressions(const Block &block, std::vector<const Expr *> &expressions)
{
    for (const Clause &clause : block.clauses)
    {
        for (const Argument &argument : clause.arguments)
        {
            if (argument.state < 0 && argument.value.type.kind != TypeKind::Vocabulary)
            {
                expressions.push_back(&argument.value);
            }
        }
    }
    for (const Transition &transition : block.transitions)
    {
        expressions.push_back(&transition.condition);
        for (const Clause &clause : transition.clauses)
        {
            for (const Argument &argument : clause.arguments)
            {
                expressions.push_back(&argument.value);
            }
        }
    }
    for (const Block &child : block.children)
    {
        CollectExpressions(child, expressions);
    }
}

struct Expected
{
    std::string_view says;
    uint32_t line = 0;
};

} // namespace

TEST_CASE("Integration: the factory compiles clean, lowers whole, and decides a frame")
{
    const std::expected<Program, Diagnostics> program = CompileExample("factory.sgl");
    REQUIRE_MESSAGE(program.has_value(), Dump(Errors(program)));
    CHECK_MESSAGE(program->warnings.empty(), Dump(program->warnings));

    Lowered lowered;
    lowered.layout = MakeLayout(*program, Robot());
    std::expected<std::vector<uint32_t>, std::string> lets = LowerLets(*program, lowered.layout, lowered.code);
    REQUIRE_MESSAGE(lets.has_value(), lets.error());
    lowered.lets = std::move(*lets);

    std::vector<const Expr *> expressions;
    CollectExpressions(program->root, expressions);
    std::vector<uint32_t> entries = lowered.lets;
    for (const Expr *expr : expressions)
    {
        std::expected<uint32_t, std::string> entry = LowerExpression(lowered.layout, *expr, lowered.code);
        REQUIRE_MESSAGE(entry.has_value(), entry.error());
        entries.push_back(*entry);
    }
    for (const uint32_t entry : entries)
    {
        const std::expected<void, std::string> verified =
            Verify(lowered.code, entry, static_cast<uint32_t>(lowered.layout.slots.size()));
        CHECK_MESSAGE(verified.has_value(), Disassemble(lowered.code, entry));
    }

    // The loader's speed, 2.0 * 0.75, is worked out at cook time.
    const Expr &speed = program->root.children[0].clauses[0].arguments[0].value;
    std::vector<Word> speedCode;
    CHECK(Disassemble(speedCode, *LowerExpression(lowered.layout, speed, speedCode)) == "0: PushFloat 1.5\n"
                                                                                       "2: Return\n");

    std::vector<Word> block = BlockOf(lowered.layout, {{"shift", FromInt(1)},
                                                       {"load", FromFloat(30.f)},
                                                       {"parts", FromInt(5)},
                                                       {"heat", FromFloat(50.f)},
                                                       {"start", FromBool(true)},
                                                       {"battery", FromFloat(0.5f)},
                                                       {"tick", FromInt(10)}});
    EvaluateLets(lowered.code, lowered.lets, lowered.layout, block);
    CHECK(block[*FindSlot(lowered.layout, "busy")] == FromBool(true));
    CHECK(block[*FindSlot(lowered.layout, "heavy")] == FromBool(true));
    CHECK(block[*FindSlot(lowered.layout, "low")] == FromBool(false));
    CHECK(block[*FindSlot(lowered.layout, "go")] == FromBool(true));
    // min(5 * 2, 6) + abs(5 - 10) % 3
    CHECK(ToInt(block[*FindSlot(lowered.layout, "effort")]) == 8);

    const Block &loader = program->root.children[0];
    std::vector<bool> decided;
    for (const Transition &transition : loader.transitions)
    {
        std::vector<Word> code;
        const uint32_t entry = *LowerExpression(lowered.layout, transition.condition, code);
        decided.push_back(ToBool(Evaluate(code, entry, block)));
    }
    CHECK(decided == std::vector<bool>{true, true, false, false, false, false});
}

TEST_CASE("Integration: the broken factory reports every mistake once, where it is")
{
    const std::array expected{
        Expected{"can't read \"shared/missing.sgl\"", 4},
        Expected{"\"parts\" is declared twice", 13},
        Expected{"\"when\" can't be a name", 14},
        Expected{"unknown type \"feelings\"", 15},
        Expected{"a const can't use \"load\"", 17},
        Expected{"a let can't hold a string", 18},
        Expected{"Shift has no value \"of\"", 19},
        Expected{"\"later\" is used before it is declared", 20},
        Expected{"unknown name \"max_heet\"", 22},
        Expected{"unknown function \"batery\"", 23},
        Expected{"this divides by zero", 24},
        Expected{"'&&' can't take an int and a float", 25},
        Expected{"this expression nests too deeply to run", 26},
        Expected{"clauses go inside a block", 28},
        Expected{"unknown signal \"bepe\" — did you mean \"beep\"?", 34},
        Expected{"a trigger can only decide transitions", 35},
        Expected{"\"after\" can't go in a node", 36},
        Expected{"mismatched types", 39},
        Expected{"\"enabled\" takes 1 values, got 2", 41},
        Expected{"unknown block kind \"nod\"", 43},
        Expected{"unknown state \"nowhere\" in this block", 44},
        Expected{"\"lost\" is unreachable from the \"loader\" machine", 45},
        Expected{"a condition must be a bool", 48},
        Expected{"unknown state \"chrage\" in machine \"loader\"", 49},
        Expected{"this transition goes to \"idle\", so it can't also leave from \"idle\"", 50},
        Expected{"\"charge\" isn't in the set it's removed from", 51},
        Expected{"\"cost\" can't go in a transition", 53},
        Expected{"a node can't go at the top level of the file", 56},
        Expected{"dock \"bay\" has no \"port\" clause", 58},
    };
    const Diagnostics errors = Errors(CompileExample("factory_broken.sgl"));
    REQUIRE_MESSAGE(errors.size() == expected.size(), Dump(errors));
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        CAPTURE(errors[i].message);
        CHECK(errors[i].message == expected[i].says);
        CHECK(errors[i].where.line == expected[i].line);
    }
}
