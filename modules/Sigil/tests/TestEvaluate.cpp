/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestEvaluate.cpp
/// @brief Expressions compiled to bytecode give the values the language
///        promises: every operator on every type, ints that wrap, division by
///        zero that gives 0, NaN that orders nothing, && and || that skip
///        their right side, and lets that each frame read the lets before them.

#include "BytecodeTesting.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

using namespace Assisi::Sigil;
using namespace Assisi::Sigil::Testing;

namespace
{

/// Params of every type, so no operator below is folded away at cook time.
constexpr std::string_view kParams = "enum Mode { idle, walk }\n"
                                     "param i: int;\nparam j: int;\nparam f: float;\nparam g: float;\n"
                                     "param a: bool;\nparam b: bool;\nparam t: trigger;\nparam m: Mode;\n";

struct Case
{
    std::string_view expression;
    Word expected;
};

constexpr int32_t kIntMax = std::numeric_limits<int32_t>::max();
constexpr int32_t kIntMin = std::numeric_limits<int32_t>::min();

} // namespace

TEST_CASE("Evaluate: every operator on every type")
{
    const std::array cases{
        Case{"i + j", FromInt(9)},
        Case{"i - j", FromInt(5)},
        Case{"j - i", FromInt(-5)},
        Case{"i * j", FromInt(14)},
        Case{"i / j", FromInt(3)},
        Case{"-i / j", FromInt(-3)},
        Case{"i % j", FromInt(1)},
        Case{"-i % j", FromInt(-1)},
        Case{"-i", FromInt(-7)},
        Case{"i == j", FromBool(false)},
        Case{"i == 7", FromBool(true)},
        Case{"i != j", FromBool(true)},
        Case{"i < j", FromBool(false)},
        Case{"j < i", FromBool(true)},
        Case{"i <= j", FromBool(false)},
        Case{"j <= 2", FromBool(true)},
        Case{"i > j", FromBool(true)},
        Case{"j > i", FromBool(false)},
        Case{"i >= 7", FromBool(true)},
        Case{"j >= i", FromBool(false)},
        Case{"f + g", FromFloat(9.5f)},
        Case{"f - g", FromFloat(5.5f)},
        Case{"f * g", FromFloat(15.f)},
        Case{"f / g", FromFloat(3.75f)},
        Case{"-f", FromFloat(-7.5f)},
        Case{"f == g", FromBool(false)},
        Case{"f != g", FromBool(true)},
        Case{"f < g", FromBool(false)},
        Case{"g < f", FromBool(true)},
        Case{"f <= 7.5", FromBool(true)},
        Case{"f <= g", FromBool(false)},
        Case{"f > g", FromBool(true)},
        Case{"g > f", FromBool(false)},
        Case{"f >= 7.5", FromBool(true)},
        Case{"g >= f", FromBool(false)},
        Case{"i + f", FromFloat(14.5f)},
        Case{"i / g", FromFloat(3.5f)},
        Case{"a && b", FromBool(false)},
        Case{"a && a", FromBool(true)},
        Case{"a || b", FromBool(true)},
        Case{"b || b", FromBool(false)},
        Case{"!a", FromBool(false)},
        Case{"!b", FromBool(true)},
        Case{"a == b", FromBool(false)},
        Case{"a != b", FromBool(true)},
        Case{"t && a", FromBool(true)},
        Case{"t == a", FromBool(true)},
        Case{"m == Mode.walk", FromBool(true)},
        Case{"m == Mode.idle", FromBool(false)},
        Case{"m != Mode.idle", FromBool(true)},
        Case{"abs(-i)", FromInt(7)},
        Case{"abs(-f)", FromFloat(7.5f)},
        Case{"min(i, j)", FromInt(2)},
        Case{"min(f, g)", FromFloat(2.f)},
        Case{"max(i, j)", FromInt(7)},
        Case{"max(f, g)", FromFloat(7.5f)},
        Case{"clamp(i, 0, 5)", FromInt(5)},
        Case{"clamp(j, 3, 5)", FromInt(3)},
        Case{"clamp(j, 0, 5)", FromInt(2)},
        Case{"clamp(f, 0.0, 5.0)", FromFloat(5.f)},
        Case{"clamp(g, 3.0, 5.0)", FromFloat(3.f)},
        Case{"battery() * 2.0", FromFloat(0.5f)},
    };
    for (const Case &c : cases)
    {
        CAPTURE(c.expression);
        const Word value = Run(kParams, c.expression,
                               {{"i", FromInt(7)},
                                {"j", FromInt(2)},
                                {"f", FromFloat(7.5f)},
                                {"g", FromFloat(2.f)},
                                {"a", FromBool(true)},
                                {"b", FromBool(false)},
                                {"t", FromBool(true)},
                                {"m", FromInt(1)},
                                {"battery", FromFloat(0.25f)}});
        CHECK(value == c.expected);
    }
}

TEST_CASE("Evaluate: ints wrap, and dividing by zero gives 0")
{
    const Values high{{"i", FromInt(kIntMax)}, {"j", FromInt(0)}};
    CHECK(ToInt(Run(kParams, "i + 1", high)) == kIntMin);
    CHECK(ToInt(Run(kParams, "i * 2", high)) == -2);
    CHECK(ToInt(Run(kParams, "i / j", high)) == 0);
    CHECK(ToInt(Run(kParams, "i % j", high)) == 0);

    const Values low{{"i", FromInt(kIntMin)}};
    CHECK(ToInt(Run(kParams, "i - 1", low)) == kIntMax);
    CHECK(ToInt(Run(kParams, "-i", low)) == kIntMin);
    CHECK(ToInt(Run(kParams, "abs(i)", low)) == kIntMin);
    CHECK(ToInt(Run(kParams, "i / -1", low)) == kIntMin);
    CHECK(ToInt(Run(kParams, "i % -1", low)) == 0);

    CHECK(ToFloat(Run(kParams, "f / g", {{"f", FromFloat(3.f)}, {"g", FromFloat(0.f)}})) == 0.f);
    CHECK(ToFloat(Run(kParams, "f / g", {{"f", FromFloat(3.f)}, {"g", FromFloat(-0.f)}})) == 0.f);
}

TEST_CASE("Evaluate: NaN equals and orders nothing, and min, max and clamp keep their first value")
{
    const Word nan = FromFloat(std::numeric_limits<float>::quiet_NaN());
    const Values values{{"f", nan}, {"g", FromFloat(1.f)}};
    CHECK(Run(kParams, "f == f", values) == FromBool(false));
    CHECK(Run(kParams, "f != f", values) == FromBool(true));
    CHECK(Run(kParams, "f < g", values) == FromBool(false));
    CHECK(Run(kParams, "f >= g", values) == FromBool(false));
    CHECK(std::isnan(ToFloat(Run(kParams, "min(f, g)", values))));
    CHECK(std::isnan(ToFloat(Run(kParams, "max(f, g)", values))));
    CHECK(ToFloat(Run(kParams, "min(g, f)", values)) == 1.f);
    CHECK(ToFloat(Run(kParams, "max(g, f)", values)) == 1.f);

    CHECK(Run(kParams, "f == g", {{"f", FromFloat(-0.f)}, {"g", FromFloat(0.f)}}) == FromBool(true));
    // Bounds the wrong way round give the upper one rather than anything undefined.
    CHECK(ToFloat(Run(kParams, "clamp(f, 5.0, 1.0)", {{"f", FromFloat(3.f)}})) == 1.f);
    CHECK(ToInt(Run(kParams, "clamp(i, 5, 1)", {{"i", FromInt(3)}})) == 1);
}

TEST_CASE("Evaluate: && and || skip their right side when the left decides")
{
    // Were the jump not taken, the right side would run and leave true.
    const std::array<Word, 6> skipsAnd{MakeWord(Opcode::PushBool), FromBool(false), MakeWord(Opcode::AndJump, 2),
                                       MakeWord(Opcode::PushBool), FromBool(true),  MakeWord(Opcode::Return)};
    REQUIRE(Verify(skipsAnd, 0, 0).has_value());
    CHECK(Evaluate(skipsAnd, 0, {}) == FromBool(false));

    const std::array<Word, 6> skipsOr{MakeWord(Opcode::PushBool), FromBool(true),  MakeWord(Opcode::OrJump, 2),
                                      MakeWord(Opcode::PushBool), FromBool(false), MakeWord(Opcode::Return)};
    REQUIRE(Verify(skipsOr, 0, 0).has_value());
    CHECK(Evaluate(skipsOr, 0, {}) == FromBool(true));

    // A compiled && jumps from after its left side to past its right side.
    CHECK(CodeOf(kParams, "a && i > j") == "0: Load 4\n"
                                           "1: AndJump -> 5\n"
                                           "2: Load 0\n"
                                           "3: Load 1\n"
                                           "4: GreaterInt\n"
                                           "5: Return\n");
    CHECK(CodeOf(kParams, "a || b") == "0: Load 4\n"
                                       "1: OrJump -> 3\n"
                                       "2: Load 5\n"
                                       "3: Return\n");
}

TEST_CASE("Evaluate: what is known at cook time is folded, the way the game would work it out")
{
    CHECK(CodeOf(kParams, "f > 2.0 * 3.0") == "0: Load 2\n"
                                              "1: PushFloat 6\n"
                                              "3: GreaterFloat\n"
                                              "4: Return\n");
    CHECK(CodeOf(kParams, "abs(-2) + i") == "0: PushInt 2\n"
                                            "2: Load 0\n"
                                            "3: AddInt\n"
                                            "4: Return\n");
    CHECK(CodeOf(kParams, "1 + 2 > 2 && !false") == "0: PushBool true\n"
                                                    "2: Return\n");
    // Folding wraps as the game does, not refusing as a const would.
    CHECK(CodeOf(kParams, "2147483647 + 1 + i") == "0: PushInt -2147483648\n"
                                                   "2: Load 0\n"
                                                   "3: AddInt\n"
                                                   "4: Return\n");
}

TEST_CASE("Evaluate: params, then the vocabulary's functions, then lets; a let reads the lets before it")
{
    const Lowered lowered = LowerRobot("param pace: float;\nparam s: signal;\nlet fast = pace > battery();\n"
                                       "let tired = fast && battery() < 0.5;\n");
    const Layout &layout = lowered.layout;
    REQUIRE(layout.slots.size() == 6);
    const std::array<std::string_view, 6> names{"pace", "s", "tick", "battery", "fast", "tired"};
    for (std::size_t slot = 0; slot < names.size(); ++slot)
    {
        CHECK(layout.slots[slot].name == names[slot]);
    }
    CHECK(layout.slots[1].type == SlotType::None);
    CHECK(layout.slots[2].kind == SlotKind::Implied);
    CHECK(layout.slots[2].type == SlotType::Int);
    CHECK(layout.slots[4].kind == SlotKind::Let);
    CHECK(layout.LetSlot(1) == 5);

    std::vector<Word> block = BlockOf(layout, {{"pace", FromFloat(1.f)}, {"battery", FromFloat(0.25f)}});
    EvaluateLets(lowered.code, lowered.lets, layout, block);
    CHECK(block[4] == FromBool(true));
    CHECK(block[5] == FromBool(true));

    // A vocabulary-typed value has no bytecode.
    const std::expected<Program, Diagnostics> program =
        CompileRobot("use robot;\nparam s: signal;\nmachine m { node a { emit s; } }\n");
    REQUIRE(program.has_value());
    std::vector<Word> code;
    const Expr &signal = program->root.children[0].children[0].clauses[0].arguments[0].value;
    CHECK_FALSE(LowerExpression(MakeLayout(*program, Robot()), signal, code).has_value());
    CHECK(code.empty());
}

TEST_CASE("Evaluate: a call's index names the core function CoreFunction says")
{
    const std::array<std::string_view, 4> names{"abs", "min", "max", "clamp"};
    REQUIRE(CoreFunctions().size() == static_cast<std::size_t>(CoreFunction::Count_));
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        CHECK(CoreFunctions()[i].name == names[i]);
    }
}
