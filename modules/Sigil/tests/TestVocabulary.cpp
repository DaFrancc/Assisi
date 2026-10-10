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

/// Robot with a `site` clause written once at the top of a file, outside any
/// block, as @p cardinality allows.
Vocabulary RobotWithSite(Cardinality cardinality)
{
    Vocabulary robot = Robot();
    robot.clauses.push_back(ClauseSpec{.word = "site",
                                       .arguments = {{"int"}},
                                       .blocks = {},
                                       .onTransition = false,
                                       .fileLevel = true,
                                       .cardinality = cardinality});
    return robot;
}

std::expected<Program, Diagnostics> CompileWithSite(std::string_view source, Cardinality cardinality,
                                                    const Files &files = {})
{
    const std::array<Vocabulary, 1> vocabularies{RobotWithSite(cardinality)};
    return CompileSource(source, "main.sgl", vocabularies, ReaderOf(files));
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

    // The engine writes a vocabulary function's value, so nothing can be passed to it.
    Vocabulary arguments = Robot();
    arguments.functions.push_back(FunctionSpec{.name = "charge", .parameters = {"int"}, .result = "float"});
    CHECK(CheckVocabulary(arguments).error().find("can take none") != std::string::npos);
    Vocabulary signalResult = Robot();
    signalResult.functions.push_back(FunctionSpec{.name = "heard", .parameters = {}, .result = "signal"});
    CHECK(CheckVocabulary(signalResult).error().find("gives a float, an int or a bool") != std::string::npos);

    Vocabulary unknownParent = Robot();
    unknownParent.blocks.push_back(BlockKind{.name = "room", .parents = {"house"}});
    CHECK(CheckVocabulary(unknownParent).error().find("\"house\", which isn't a block kind") != std::string::npos);

    const std::array<Vocabulary, 1> vocabularies{reserved};
    const Files none;
    const std::expected<Program, Diagnostics> compiled =
        CompileSource("use robot;\n", "main.sgl", vocabularies, ReaderOf(none));
    CHECK(HasError(Errors(compiled), 1, "the robot vocabulary can't be used"));
}

TEST_CASE("Vocabulary: a file-level clause is written at the top of the file, outside any block")
{
    std::expected<Program, Diagnostics> program =
        CompileWithSite("use robot;\nsite 3;\nmachine m { node a { } }\n", Cardinality::AtMostOnce);
    REQUIRE_MESSAGE(program.has_value(), Dump(Errors(program)));
    REQUIRE(program->root.clauses.size() == 1);
    CHECK(std::get<int32_t>(program->root.clauses[0].arguments[0].value.literal) == 3);

    const Diagnostics inside =
        Errors(CompileWithSite("use robot;\nmachine m {\n    node a { site 3; }\n}\n", Cardinality::AtMostOnce));
    CHECK_MESSAGE(HasError(inside, 3, "\"site\" can't go in a node"), Dump(inside));
    CHECK_MESSAGE(HasError(inside, 3, "the top of the file"), Dump(inside));

    // Other clauses still belong in blocks.
    CHECK(HasError(Errors(CompileWithSite("use robot;\ncost 1;\n", Cardinality::AtMostOnce)), 2,
                   "clauses go inside a block"));
}

TEST_CASE("Vocabulary: a file-level clause is written as often as the vocabulary allows")
{
    const Diagnostics missing =
        Errors(CompileWithSite("use robot;\nmachine m { node a { } }\n", Cardinality::ExactlyOnce));
    CHECK_MESSAGE(HasError(missing, 1, "the file has no \"site\" clause"), Dump(missing));

    const Diagnostics twice =
        Errors(CompileWithSite("use robot;\nsite 1;\nsite 2;\n", Cardinality::AtMostOnce));
    CHECK_MESSAGE(HasError(twice, 3, "\"site\" is written twice in the file"), Dump(twice));

    // A library holds only enums and consts, so it neither needs nor takes one.
    const Files libraries{{"shared.sgl", "use robot;\nsigiltype library;\nconst k = 1;\n"}};
    std::expected<Program, Diagnostics> importing = CompileWithSite(
        "use robot;\nimport \"shared.sgl\";\nsite k;\nmachine m { node a { } }\n", Cardinality::ExactlyOnce, libraries);
    CHECK_MESSAGE(importing.has_value(), Dump(Errors(importing)));
    CHECK(HasError(Errors(CompileWithSite("use robot;\nsigiltype library;\nsite 1;\n", Cardinality::AtMostOnce)), 3,
                   "a library can't hold clauses"));
}

TEST_CASE("Vocabulary: a file-level clause goes nowhere else and names no state")
{
    CHECK(CheckVocabulary(RobotWithSite(Cardinality::ExactlyOnce)).has_value());

    Vocabulary inBlock = RobotWithSite(Cardinality::AtMostOnce);
    inBlock.clauses.back().blocks = {"node"};
    CHECK(CheckVocabulary(inBlock).error().find("only at the top of the file") != std::string::npos);

    Vocabulary onTransition = RobotWithSite(Cardinality::AtMostOnce);
    onTransition.clauses.back().onTransition = true;
    CHECK(CheckVocabulary(onTransition).error().find("only at the top of the file") != std::string::npos);

    Vocabulary namesState = RobotWithSite(Cardinality::AtMostOnce);
    namesState.clauses.back().arguments = {ArgumentSpec{.type = {}, .kind = ArgumentKind::State}};
    CHECK(CheckVocabulary(namesState).error().find("has no states to name") != std::string::npos);
}

TEST_CASE("Vocabulary: a file's imports and file-level strings are read without checking it")
{
    constexpr std::string_view kSource = "use robot;\nimport \"a.sgl\";\nimport \"b/c.sgl\";\nsite \"x.glb\";\n"
                                         "machine m { node a { cost nope; } }\n";
    CHECK(ListImports(kSource) == std::vector<std::string>{"a.sgl", "b/c.sgl"});
    CHECK(FileClauseString(kSource, "site") == std::optional<std::string>{"x.glb"});
    CHECK_FALSE(FileClauseString(kSource, "cost").has_value());
    CHECK(ListImports("use robot;\nimport").empty());
}
