/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestParser.cpp
/// @brief A file parses into blocks, clauses and transitions by its shape
/// alone, with operators nesting by precedence, and a malformed one is refused
/// with what was expected and where.

#include "SigilTesting.hpp"

#include <doctest/doctest.h>

#include <Assisi/Sigil/Compile/Lexer.hpp>
#include <Assisi/Sigil/Compile/Parser.hpp>

#include <variant>

using namespace Assisi::Sigil::Compile;
using Assisi::Sigil::Testing::Dump;
using Assisi::Sigil::Testing::HasError;

namespace
{

std::expected<Syntax::File, Diagnostics> ParseText(std::string_view source)
{
    std::expected<std::vector<Token>, Diagnostics> tokens = Lex(source, "main.sgl");
    REQUIRE(tokens.has_value());
    return Parse(*tokens, "main.sgl");
}

Syntax::File Parsed(std::string_view source)
{
    std::expected<Syntax::File, Diagnostics> file = ParseText(source);
    REQUIRE_MESSAGE(file.has_value(), Dump(file.has_value() ? Diagnostics{} : file.error()));
    return std::move(*file);
}

Diagnostics ParseErrors(std::string_view source)
{
    std::expected<Syntax::File, Diagnostics> file = ParseText(source);
    REQUIRE_FALSE(file.has_value());
    return std::move(file.error());
}

} // namespace

TEST_CASE("Parser: a file parses into its blocks, clauses and transitions")
{
    const Syntax::File file = Parsed(R"(use robot;
sigiltype library;
param p: float;
machine m {
    speed 2.0;
    node a { emit "beep", "boop"; goto b; }
    a -> b when p > 1.0 { after 0.5; };
    node b {
        node inner { }
    }
    any - a + b -> b when p < 0.0;
}
)");
    CHECK(file.use.vocabulary.name == "robot");
    REQUIRE(file.sigilType.has_value());
    CHECK(file.sigilType->name == "library");
    REQUIRE(file.declarations.size() == 1);
    CHECK(std::holds_alternative<Syntax::ParamDecl>(file.declarations[0]));

    REQUIRE(file.root.blocks.size() == 1);
    const Syntax::Block &machine = file.root.blocks[0];
    CHECK(machine.kind.name == "machine");
    CHECK(machine.name.where.line == 4);
    REQUIRE(machine.clauses.size() == 1);
    CHECK(machine.clauses[0].word.name == "speed");

    // Blocks keep their order, apart from the transitions between them.
    REQUIRE(machine.blocks.size() == 2);
    CHECK(machine.blocks[0].name.name == "a");
    CHECK(machine.blocks[0].clauses[0].arguments.size() == 2);
    CHECK(machine.blocks[1].blocks.size() == 1);

    REQUIRE(machine.transitions.size() == 2);
    const Syntax::Transition &first = machine.transitions[0];
    CHECK(first.first.name == "a");
    CHECK(first.target.name == "b");
    REQUIRE(first.clauses.size() == 1);
    CHECK(first.clauses[0].word.name == "after");
    const Syntax::Transition &second = machine.transitions[1];
    CHECK(second.fromAny);
    REQUIRE(second.steps.size() == 2);
    CHECK(second.steps[0].remove);
    CHECK_FALSE(second.steps[1].remove);
    CHECK(second.clauses.empty());
}

TEST_CASE("Parser: operators nest from loosest to tightest")
{
    const Syntax::File file = Parsed("use robot;\nlet x = !a || b && c == d < e + f * g;\n");
    const Syntax::ValueDecl &let = std::get<Syntax::ValueDecl>(file.declarations[0]);
    const Syntax::Expr &orExpr = let.value;
    REQUIRE(orExpr.kind == Syntax::ExprKind::Binary);
    CHECK(orExpr.op == TokenKind::Or);
    CHECK(orExpr.operands[0].kind == Syntax::ExprKind::Unary);
    const Syntax::Expr &andExpr = orExpr.operands[1];
    CHECK(andExpr.op == TokenKind::And);
    const Syntax::Expr &equal = andExpr.operands[1];
    CHECK(equal.op == TokenKind::Equal);
    const Syntax::Expr &less = equal.operands[1];
    CHECK(less.op == TokenKind::Less);
    const Syntax::Expr &plus = less.operands[1];
    CHECK(plus.op == TokenKind::Plus);
    CHECK(plus.operands[1].op == TokenKind::Star);
}

TEST_CASE("Parser: calls, enum values and parentheses")
{
    const Syntax::File file = Parsed("use robot;\nlet x = (clamp(a, 0, 1) + 2) * 3;\nlet y = Mode.fast;\n");
    const Syntax::Expr &times = std::get<Syntax::ValueDecl>(file.declarations[0]).value;
    CHECK(times.op == TokenKind::Star);
    const Syntax::Expr &call = times.operands[0].operands[0];
    CHECK(call.kind == Syntax::ExprKind::Call);
    CHECK(call.text == "clamp");
    CHECK(call.operands.size() == 3);
    const Syntax::Expr &member = std::get<Syntax::ValueDecl>(file.declarations[1]).value;
    CHECK(member.kind == Syntax::ExprKind::Member);
    CHECK(member.text == "Mode");
    CHECK(member.member == "fast");
}

TEST_CASE("Parser: a malformed file is refused with what was expected and where")
{
    CHECK(HasError(ParseErrors("use robot;\nparam p: float\nparam q: int;\n"), 3, "expected ';'"));
    CHECK(HasError(ParseErrors("use robot;\nmachine m {\n"), 3, "expected '}'"));
    CHECK(HasError(ParseErrors("use robot;\nmachine m { node a {} a -> a; }\n"), 2, "\"when\""));
    CHECK(HasError(ParseErrors("use robot;\nmachine m { node a {} a -> a when x { after 1.0; } }\n"), 2,
                   "at the end of the transition"));
    CHECK(HasError(ParseErrors("use robot;\nmachine m {\n};\n"), 3, "remove this ';'"));
    CHECK(HasError(ParseErrors("use robot;\nmachine m {\n  param p: float;\n}\n"), 3, "top level"));
    CHECK(HasError(ParseErrors("param p: float;\n"), 1, "starts with \"use"));
    CHECK(HasError(ParseErrors("use robot;\nuse robot;\n"), 2, "only come at the start"));
    CHECK(HasError(ParseErrors("use robot;\nlet x = when;\n"), 2, "expected a value"));
}

TEST_CASE("Parser: after an error it carries on, and reports the next one too")
{
    const Diagnostics errors = ParseErrors("use robot;\nparam p float;\nparam q: int\nlet r = 1;\n");
    CHECK(HasError(errors, 2, "expected ':'"));
    CHECK(HasError(errors, 4, "expected ';'"));
}

TEST_CASE("Parser: blocks nested too deep are refused, not a crash")
{
    std::string source = "use robot;\n";
    for (uint32_t depth = 0; depth <= kMaxBlockDepth; ++depth)
    {
        source += std::format("node n{} {{\n", depth);
    }
    for (uint32_t depth = 0; depth <= kMaxBlockDepth; ++depth)
    {
        source += "}\n";
    }
    const Diagnostics errors = ParseErrors(source);
    CHECK(std::ranges::any_of(errors, [](const Diagnostic &error) {
        return error.message.find("nest more than") != std::string::npos;
    }));
}

TEST_CASE("Parser: the use line can be read without compiling the file")
{
    CHECK(ReadUseLine("// a comment\nuse robot;\nparam p: float;") == std::optional<std::string>{"robot"});
    CHECK_FALSE(ReadUseLine("param p: float;").has_value());
    CHECK_FALSE(ReadUseLine("use robot").has_value());
}
