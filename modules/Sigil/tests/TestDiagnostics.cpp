/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestDiagnostics.cpp
/// @brief What a person reads when a file is wrong: each error shows its lines
/// with the place underlined and labelled, related places marked beside it,
/// and help below, in color for a terminal; errors come in the order of the
/// file; one mistake isn't reported again where it's used; and nothing is
/// called unused when errors may have hidden its use.

#include "SigilTesting.hpp"

#include <doctest/doctest.h>

using namespace Assisi::Sigil::Testing;

TEST_CASE("Diagnostics: an error underlines its place and a related one, each labelled, with help below")
{
    const Diagnostics errors = Errors(CompileRobot("use robot;\nparam p: bool;\nmachine m {\n    node a { }\n"
                                                   "    node b { }\n    a -> b when p;\n\tany + a -> a when p;\n}\n"));
    REQUIRE_MESSAGE(errors.size() == 1, Dump(errors));
    CHECK(Format(errors[0]) ==
          "error: this transition goes to \"a\", so it can't also leave from \"a\"\n"
          " --> main.sgl:7:2\n"
          "  |\n"
          "7 | \tany + a -> a when p;\n"
          "  | \t^^^^^^^    - and it goes to \"a\"\n"
          "  | \t|\n"
          "  | \tthese include \"a\"\n"
          "  |\n"
          "  = help: to restart \"a\" while it's playing, write \"a -> a\" as a transition of its own\n");
}

TEST_CASE("Diagnostics: in color, the parts are painted, and plain has no escapes")
{
    const Diagnostics errors = Errors(CompileRobot("use robot;\nlet x = y;\n"));
    REQUIRE(errors.size() == 1);
    const std::string colored = Format(errors[0], Style::Color);
    CHECK(colored.find("\x1b[1;31merror:\x1b[0m") != std::string::npos);
    CHECK(colored.find("\x1b[1;31m^\x1b[0m") != std::string::npos);
    CHECK(Format(errors[0], Style::Plain).find('\x1b') == std::string::npos);
}

TEST_CASE("Diagnostics: errors come in the order they appear in the file")
{
    const Diagnostics errors = Errors(CompileRobot("use robot;\nparam p: bool;\nmachine m {\n    node a { }\n"
                                                   "    node b { }\n    a -> a when q;\n}\n"));
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].where.line == 5);
    CHECK(errors[1].where.line == 6);
}

TEST_CASE("Diagnostics: a name that was refused isn't reported again where it's used")
{
    const Diagnostics errors =
        Errors(CompileRobot("use robot;\nparam cost: float;\nlet a = cost > 1.0;\nlet b = cost < 2.0;\n"));
    REQUIRE_MESSAGE(errors.size() == 1, Dump(errors));
    CHECK(HasError(errors, 2, "word of the robot vocabulary"));
}

TEST_CASE("Diagnostics: a transition's condition is checked even when its states are wrong")
{
    const Diagnostics errors = Errors(CompileRobot("use robot;\nmachine m {\n    node a { }\n"
                                                   "    a -> nowhere when speedd > 1;\n}\n"));
    CHECK_MESSAGE(HasError(errors, 4, "unknown state \"nowhere\""), Dump(errors));
    CHECK(HasError(errors, 4, "unknown name \"speedd\""));
}

TEST_CASE("Diagnostics: nothing is called unused in a file that has errors")
{
    // The block of a kind nobody declared is never checked, so its use of k is never counted.
    const Diagnostics errors =
        Errors(CompileRobot("use robot;\nparam k: int;\nmachine m {\n    nod x { cost k; }\n}\n"));
    REQUIRE_FALSE(errors.empty());
    CHECK(std::ranges::none_of(errors, [](const Diagnostic &diagnostic) {
        return diagnostic.severity == Severity::Warning;
    }));
}
