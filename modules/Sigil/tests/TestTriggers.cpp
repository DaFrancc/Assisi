/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestTriggers.cpp
/// @brief A trigger, and anything that reads one, only decides transitions: in
/// a `when` it's allowed and recorded as used up by the transition, and in a
/// clause value it's refused, whether read directly or through a let.

#include "SigilTesting.hpp"

#include <doctest/doctest.h>

using namespace Assisi::Sigil::Testing;

TEST_CASE("Triggers: a trigger in a clause value is refused")
{
    const Diagnostics errors = Errors(CompileRobot(
        "use robot;\nparam t: trigger;\nmachine m {\n    node a {\n        enabled t;\n    }\n}\n"));
    CHECK_MESSAGE(HasError(errors, 5, "can only decide transitions"), Dump(errors));
}

TEST_CASE("Triggers: a let that reads a trigger can decide transitions and nothing else")
{
    const Diagnostics errors = Errors(CompileRobot("use robot;\nparam t: trigger;\nparam ready: bool;\n"
                                                   "let go = t && ready;\nmachine m {\n    node a {\n"
                                                   "        enabled go;\n    }\n}\n"));
    CHECK_MESSAGE(HasError(errors, 7, "can only decide transitions"), Dump(errors));

    std::expected<Program, Diagnostics> program =
        CompileRobot("use robot;\nparam ready: bool;\nparam t: trigger;\nlet go = t && ready;\nmachine m {\n"
                     "    node a { }\n    node b { }\n    a -> b when go;\n}\n");
    REQUIRE_MESSAGE(program.has_value(), Dump(Errors(program)));
    CHECK(program->lets[0].whenOnly);
    const Transition &transition = program->root.children[0].transitions[0];
    // The trigger read through the let is the one firing uses up.
    CHECK(transition.triggersRead == std::vector<uint32_t>{1});
}

TEST_CASE("Triggers: a when-only function is a trigger too")
{
    CHECK(HasError(Errors(CompileRobot("use robot;\nlet n = tick();\nmachine m {\n    node a { cost n; }\n}\n")), 4,
                   "can only decide transitions"));
    std::expected<Program, Diagnostics> program = CompileRobot(
        "use robot;\nmachine m {\n    node a { }\n    node b { }\n    a -> b when tick() > 3;\n}\n");
    CHECK_MESSAGE(program.has_value(), Dump(Errors(program)));
}

TEST_CASE("Triggers: a condition must be a bool, and a trigger is one")
{
    CHECK(HasError(Errors(CompileRobot(
                       "use robot;\nparam f: float;\nmachine m {\n    node a { }\n    node b { }\n    a -> b when f;\n}\n")),
                   6, "must be a bool, got a float"));
    std::expected<Program, Diagnostics> program = CompileRobot(
        "use robot;\nparam t: trigger;\nmachine m {\n    node a { }\n    node b { }\n    a -> b when t;\n}\n");
    REQUIRE_MESSAGE(program.has_value(), Dump(Errors(program)));
    CHECK(program->root.children[0].transitions[0].triggersRead == std::vector<uint32_t>{0});
}
