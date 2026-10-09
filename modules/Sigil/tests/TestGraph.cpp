/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestGraph.cpp
/// @brief States are named within their block, start from the first, and must
/// all be reachable from it; transitions resolve their source sets, where `any`
/// leaves out the target and only `x -> x` restarts a state.

#include "SigilTesting.hpp"

#include <doctest/doctest.h>

using namespace Assisi::Sigil::Testing;

namespace
{

Program Compiled(std::string_view source)
{
    std::expected<Program, Diagnostics> program = CompileRobot(source);
    REQUIRE_MESSAGE(program.has_value(), Dump(Errors(program)));
    return std::move(*program);
}

constexpr std::string_view kHeader = "use robot;\nparam p: bool;\n";

std::string WithHeader(std::string_view body)
{
    return std::string{kHeader} + std::string{body};
}

} // namespace

TEST_CASE("Graph: two states of one name in one block are refused; in two blocks they're fine")
{
    const Diagnostics errors = Errors(CompileRobot(WithHeader("machine m {\n    node a { }\n    node a { }\n}\n")));
    CHECK_MESSAGE(HasError(errors, 5, "there is already a node \"a\" here, on line 4"), Dump(errors));

    const Program program = Compiled(WithHeader("machine m { node a { } }\nmachine n { node a { } }\n"
                                                "machine o { node x { } node y { } x -> y when p; }\n"));
    CHECK(program.root.children.size() == 3);
}

TEST_CASE("Graph: a state nothing leads to from the first is refused")
{
    const Diagnostics errors =
        Errors(CompileRobot(WithHeader("machine m {\n    node a { }\n    node b { }\n    node c { }\n"
                                       "    a -> b when p;\n}\n")));
    CHECK_MESSAGE(HasError(errors, 6, "\"c\" can never be reached from \"a\", the first state of machine \"m\""),
                  Dump(errors));
    CHECK_FALSE(HasError(errors, 5, "never be reached"));

    // A clause naming a state leads to it as well.
    const Program reached =
        Compiled(WithHeader("machine m {\n    node a { goto c; }\n    node c { }\n}\n"));
    CHECK(reached.root.children[0].children[0].clauses[0].arguments[0].state == 1);
}

TEST_CASE("Graph: states nested in a state are checked the same way")
{
    const Diagnostics errors = Errors(CompileRobot(
        WithHeader("machine m {\n    node outer {\n        node in1 { }\n        node in2 { }\n    }\n}\n")));
    CHECK(HasError(errors, 6, "\"in2\" can never be reached from \"in1\", the first state of node \"outer\""));
}

TEST_CASE("Graph: a transition can name a state written below it")
{
    const Program program = Compiled(WithHeader("machine m {\n    a -> b when p;\n    node a { }\n    node b { }\n}\n"));
    const Transition &transition = program.root.children[0].transitions[0];
    CHECK(transition.sources == std::vector<uint32_t>{0});
    CHECK(transition.target == 1);
}

TEST_CASE("Graph: any is every other state, and + and - change the set in order")
{
    const Program program =
        Compiled(WithHeader("machine m {\n    node a { }\n    node b { }\n    node c { }\n    node d { }\n"
                            "    any -> d when p;\n    any - b -> c when p;\n    a + c -> b when p;\n    a -> b when p;\n"
                            "    d -> d when p;\n}\n"));
    const std::vector<Transition> &transitions = program.root.children[0].transitions;
    CHECK(transitions[0].sources == std::vector<uint32_t>{0, 1, 2});
    CHECK(transitions[1].sources == std::vector<uint32_t>{0, 3});
    CHECK(transitions[4].sources == std::vector<uint32_t>{3});
    CHECK(transitions[4].target == 3);
}

TEST_CASE("Graph: a source set is refused when it doesn't make sense")
{
    const std::string states = "machine m {\n    node a { }\n    node b { }\n    a -> b when p;\n";
    CHECK(HasError(Errors(CompileRobot(WithHeader(states + "    any + b -> b when p;\n}\n"))), 7, "restart"));
    CHECK(HasError(Errors(CompileRobot(WithHeader(states + "    a + b -> b when p;\n}\n"))), 7, "restart"));
    CHECK(HasError(Errors(CompileRobot(WithHeader(states + "    any - c -> b when p;\n}\n"))), 7,
                   "unknown state \"c\""));
    CHECK(HasError(Errors(CompileRobot(WithHeader(states + "    a - b -> b when p;\n}\n"))), 7,
                   "\"b\" isn't in the set it's removed from"));
    CHECK(HasError(Errors(CompileRobot(WithHeader(states + "    a + a -> b when p;\n}\n"))), 7,
                   "\"a\" is already in the set"));
    CHECK(HasError(Errors(CompileRobot(WithHeader(states + "    a - a -> b when p;\n}\n"))), 7, "from no state"));
    CHECK(HasError(Errors(CompileRobot(WithHeader(states + "    a -> bb when p;\n}\n"))), 7,
                   "unknown state \"bb\" in machine \"m\""));
}

TEST_CASE("Graph: transitions only go in a block that holds states")
{
    CHECK(HasError(Errors(CompileRobot(WithHeader("dock d {\n    port 1;\n    a -> b when p;\n}\n"))), 5,
                   "transitions can't go in dock \"d\""));
}
