/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestVocabulary.cpp
/// @brief A file is checked against the vocabulary its `use` line names: its
/// block kinds and where they go, its clauses with their places, values and
/// counts, and its types' own checks. A vocabulary that clashes with the core
/// is refused before any file is.

#include "SigilTesting.hpp"

#include <doctest/doctest.h>

using namespace Assisi::Sigil::Testing;

namespace
{

constexpr std::string_view kHeader = "use robot;\n";

Diagnostics ErrorsOf(std::string_view body)
{
    return Errors(CompileRobot(std::string{kHeader} + std::string{body}));
}

} // namespace

TEST_CASE("Vocabulary: a vocabulary's type checks the strings written for it")
{
    std::expected<Program, Diagnostics> program = CompileRobot("use robot;\nmachine m { node a { emit \"beep\"; } }\n");
    REQUIRE_MESSAGE(program.has_value(), Dump(Errors(program)));
    const Argument &argument = program->root.children[0].children[0].clauses[0].arguments[0];
    CHECK(argument.value.type.kind == TypeKind::Vocabulary);

    const Diagnostics errors = ErrorsOf("machine m {\n    node a { emit \"bepe\"; }\n}\n");
    CHECK_MESSAGE(HasError(errors, 3, "unknown signal \"bepe\" — did you mean \"beep\"?"), Dump(errors));

    // A const holding the string is checked where it's used as one.
    CHECK(HasError(ErrorsOf("const s = \"whir\";\nmachine m {\n    node a { emit s; }\n}\n"), 4,
                   "did you mean \"whirr\""));
}

TEST_CASE("Vocabulary: a clause goes only where the vocabulary puts it")
{
    CHECK(HasError(ErrorsOf("machine m {\n    node a { after 1.0; }\n}\n"), 3, "\"after\" can't go in a node"));
    CHECK(HasError(ErrorsOf("machine m {\n    node a { }\n    node b { }\n    a -> b when true { cost 1; };\n}\n"), 5,
                   "\"cost\" can't go in a transition"));
    CHECK(HasError(ErrorsOf("speed 1.0;\n"), 2, "clauses go inside a block"));
    CHECK(HasError(ErrorsOf("machine m {\n    node a { emitt \"beep\"; }\n}\n"), 3,
                   "did you mean \"emit\"?"));
}

TEST_CASE("Vocabulary: a clause takes the values the vocabulary says")
{
    CHECK(HasError(ErrorsOf("machine m {\n    node a { goto 1; }\n}\n"), 3, "the name of a state"));
    CHECK(HasError(ErrorsOf("machine m {\n    node a { cost 1.5; }\n}\n"), 3, "expected an int, found a float"));
    CHECK(HasError(ErrorsOf("machine m {\n    node a { cost 1, 2; }\n}\n"), 3, "takes 1 values, got 2"));
}

TEST_CASE("Vocabulary: a clause is written as often as the vocabulary allows")
{
    CHECK(HasError(ErrorsOf("machine m {\n    node a {\n        cost 1;\n        cost 2;\n    }\n}\n"), 5,
                   "\"cost\" is written twice in a node"));
    CHECK(HasError(ErrorsOf("dock d { }\n"), 2, "dock \"d\" has no \"port\" clause"));
    std::expected<Program, Diagnostics> program =
        CompileRobot("use robot;\nmachine m { node a { emit \"beep\"; emit \"boop\"; } }\n");
    CHECK_MESSAGE(program.has_value(), Dump(Errors(program)));
}

TEST_CASE("Vocabulary: a block kind goes only where the vocabulary puts it")
{
    CHECK(HasError(ErrorsOf("node a { }\n"), 2, "a node can't go at the top level of the file"));
    CHECK(HasError(ErrorsOf("machine m {\n    dock d { port 1; }\n}\n"), 3, "a dock can't go in a machine"));
    CHECK(HasError(ErrorsOf("machine m {\n    nod a { }\n}\n"), 3, "did you mean \"node\"?"));
}

TEST_CASE("Vocabulary: the use line names one this build has, and nothing else is checked until it does")
{
    const Diagnostics errors = Errors(CompileRobot("use robbot;\nparam x: nonsense;\n"));
    REQUIRE(errors.size() == 1);
    CHECK(HasError(errors, 1, "unknown vocabulary \"robbot\""));
    CHECK(HasError(errors, 1, "did you mean \"robot\"?"));
}

TEST_CASE("Vocabulary: one whose words clash with the core or each other is refused")
{
    CHECK(CheckVocabulary(Robot()).has_value());

    Vocabulary reserved = Robot();
    reserved.clauses.push_back(ClauseSpec{.word = "when", .arguments = {}, .blocks = {"node"}});
    CHECK(CheckVocabulary(reserved).error().find("core Sigil word") != std::string::npos);

    Vocabulary twice = Robot();
    twice.clauses.push_back(ClauseSpec{.word = "node", .arguments = {}, .blocks = {"node"}});
    CHECK(CheckVocabulary(twice).error().find("declared twice") != std::string::npos);

    Vocabulary unknownParent = Robot();
    unknownParent.blocks.push_back(BlockKind{.name = "room", .parents = {"house"}});
    CHECK(CheckVocabulary(unknownParent).error().find("\"house\", which isn't a block kind") != std::string::npos);

    const std::array<Vocabulary, 1> vocabularies{reserved};
    const Files none;
    const std::expected<Program, Diagnostics> compiled =
        CompileSource("use robot;\n", "main.sgl", vocabularies, ReaderOf(none));
    CHECK(HasError(Errors(compiled), 1, "the robot vocabulary can't be used"));
}
