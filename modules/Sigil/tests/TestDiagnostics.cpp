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
          "help: leave \"a\" out of the set, and restart it in a transition of its own\n"
          "  |\n"
          "7 ~ \tany -> a when p;\n"
          "  + \ta -> a when p;\n");
}

TEST_CASE("Diagnostics: a one-line fix shows the line fixed, with ~ under what it replaced")
{
    const Diagnostics errors = Errors(CompileRobot("use robot;\nlet label = \"beep\";\n"));
    REQUIRE(errors.size() == 1);
    CHECK(Format(errors[0]).ends_with("help: make \"label\" a const\n"
                                      "  |\n"
                                      "2 | const label = \"beep\";\n"
                                      "  | ~~~~~\n"));

    const Diagnostics constant = Errors(CompileRobot("use robot;\nparam pace: float;\nconst fast = pace * 2.0;\n"));
    REQUIRE(constant.size() == 1);
    REQUIRE(constant[0].suggestions.size() == 1);
    const Edit &edit = constant[0].suggestions[0].edits.at(0);
    CHECK(edit.text == "let");
    CHECK(edit.span.where.line == 3);
    CHECK(edit.span.where.column == 1);
    CHECK(edit.span.length == 5);
}

TEST_CASE("Diagnostics: a declaration used too early is shown moved above its use")
{
    const Diagnostics errors = Errors(CompileRobot("use robot;\nlet ready = later > 1;\nlet b = 3;\nlet later = 2;\n"));
    REQUIRE(errors.size() == 1);
    CHECK_MESSAGE(Format(errors[0]).ends_with("help: move the declaration above where it's used\n"
                                              "  |\n"
                                              "  + let later = 2;\n"
                                              "2 | let ready = later > 1;\n"
                                              "...\n"
                                              "4 - let later = 2;\n"),
                  Format(errors[0]));

    // Sharing a line with something else, the declaration can't be moved whole,
    // so the advice stays in words.
    const Diagnostics shared = Errors(CompileRobot("use robot;\nlet a = b; let b = 1;\n"));
    REQUIRE(shared.size() == 1);
    CHECK(shared[0].suggestions.empty());
    CHECK(shared[0].help == "move the declaration above where it's used");
}

TEST_CASE("Diagnostics: a restart in a set keeps the rest of the set and moves the restart to its own line")
{
    const std::string_view states = "use robot;\nparam p: bool;\nmachine m {\n    node a { }\n    node b { }\n"
                                    "    node c { }\n    a -> b when p;\n    b -> c when p;\n";
    const Diagnostics first = Errors(CompileRobot(std::string{states} + "    a + c + b -> a when p;\n}\n"));
    REQUIRE_MESSAGE(first.size() == 1, Dump(first));
    REQUIRE(first[0].suggestions.size() == 1);
    // The set started with the target, so the next state added starts it instead.
    CHECK(first[0].suggestions[0].edits[0].text == "c + b");
    CHECK(first[0].suggestions[0].edits[1].text == "    a -> a when p;");

    // A state removed from the set stays removed.
    const Diagnostics removed = Errors(CompileRobot(std::string{states} + "    any - c + a -> a when p;\n}\n"));
    REQUIRE_MESSAGE(removed.size() == 1, Dump(removed));
    REQUIRE(removed[0].suggestions.size() == 1);
    CHECK(removed[0].suggestions[0].edits[0].text == "any - c");
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
