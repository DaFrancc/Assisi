/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestChecker.cpp
/// @brief Names resolve in one namespace with no collisions, values are typed
/// with ints widening to floats and nothing narrowing, consts are worked out
/// when the file compiles, and what nothing uses is warned about.

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

} // namespace

TEST_CASE("Checker: a value of the wrong type is refused where it is written")
{
    const Diagnostics errors = Errors(CompileRobot("use robot;\nparam x: float;\nlet y: int = x;\n"));
    CHECK_MESSAGE(HasError(errors, 3, "expected an int, got a float"), Dump(errors));
}

TEST_CASE("Checker: an int widens to a float, and a float never narrows")
{
    const Program program = Compiled("use robot;\nlet z = 1 + 2.0;\nlet w: float = 1;\nlet u = z > w;\n");
    REQUIRE(program.lets.size() == 3);
    CHECK(program.lets[0].type.kind == TypeKind::Float);
    CHECK(program.lets[0].value.operands[0].kind == ExprKind::Widen);
    CHECK(program.lets[1].type.kind == TypeKind::Float);
    CHECK(program.lets[1].value.kind == ExprKind::Widen);

    CHECK(HasError(Errors(CompileRobot("use robot;\nlet n: int = 2.0;\n")), 2, "expected an int, got a float"));
}

TEST_CASE("Checker: operators take only the types that make sense for them")
{
    CHECK(HasError(Errors(CompileRobot("use robot;\nconst r = 2.5 % 2;\n")), 2, "'%' can't take a float and an int"));
    CHECK(HasError(Errors(CompileRobot("use robot;\nparam b: bool;\nlet x = b + 1;\n")), 3, "'+' can't take"));
    CHECK(HasError(Errors(CompileRobot("use robot;\nparam f: float;\nlet x = !f;\n")), 3, "'!' needs a bool"));
    CHECK(HasError(Errors(CompileRobot("use robot;\nparam f: float;\nlet x = f && true;\n")), 3, "'&&' can't take"));
}

TEST_CASE("Checker: an enum compares with its own values and nothing else")
{
    const Diagnostics errors = Errors(CompileRobot(
        "use robot;\nenum Mode { slow, fast }\nenum Side { left, right }\nparam m: Mode;\n"
        "let ok = m == Mode.fast;\nlet number = m == 1;\nlet other = m == Side.left;\nlet typo = m == Mode.fsat;\n"));
    CHECK_FALSE(HasError(errors, 5, ""));
    CHECK(HasError(errors, 6, "can't take a Mode and an int"));
    CHECK(HasError(errors, 7, "can't take a Mode and a Side"));
    CHECK(HasError(errors, 8, "Mode has no value \"fsat\" — did you mean \"fast\"?"));
}

TEST_CASE("Checker: consts and lets must be declared before they're used")
{
    const Diagnostics errors = Errors(CompileRobot("use robot;\nlet a = b;\nlet b = 1;\n"));
    CHECK(HasError(errors, 2, "\"b\" is used before it is declared, on line 3"));
}

TEST_CASE("Checker: a const can only use what the cook knows")
{
    CHECK(HasError(Errors(CompileRobot("use robot;\nparam p: float;\nconst c = p * 2.0;\n")), 3,
                   "a const can't use \"p\", which is a param"));
    CHECK(HasError(Errors(CompileRobot("use robot;\nconst c = battery();\n")), 2, "a const can't call battery()"));
    CHECK(HasError(Errors(CompileRobot("use robot;\nconst c = 1 / 0;\n")), 2, "divides by zero"));
}

TEST_CASE("Checker: consts are worked out when the file compiles, and used as their values")
{
    const Program program =
        Compiled("use robot;\nconst w = 1.5;\nconst r = w * 3;\nconst q = 7 / 2;\nconst c = clamp(9, 0, 4);\n"
                 "param p: float;\nlet m = p > r;\n");
    REQUIRE(program.consts.size() == 4);
    CHECK(std::get<float>(program.consts[1].value) == doctest::Approx(4.5f));
    CHECK(std::get<int32_t>(program.consts[2].value) == 3);
    CHECK(std::get<int32_t>(program.consts[3].value) == 4);
    const Expr &compare = program.lets[0].value;
    REQUIRE(compare.operands.size() == 2);
    CHECK(compare.operands[1].kind == ExprKind::Literal);
    CHECK(std::get<float>(compare.operands[1].literal) == doctest::Approx(4.5f));
}

TEST_CASE("Checker: an unknown name suggests the closest one")
{
    const Diagnostics errors = Errors(CompileRobot("use robot;\nparam walk: float;\nlet x = wlak > 1.0;\n"));
    CHECK(HasError(errors, 3, "unknown name \"wlak\" — did you mean \"walk\"?"));
    CHECK(HasError(Errors(CompileRobot("use robot;\nparam x: flaot;\n")), 2, "did you mean \"float\""));
    CHECK(HasError(Errors(CompileRobot("use robot;\nlet x = clmap(1, 2, 3);\n")), 2, "did you mean \"clamp\""));
}

TEST_CASE("Checker: functions work out their result from their arguments")
{
    const Program program = Compiled("use robot;\nlet i = min(1, 2);\nlet f = min(1, 2.0);\nlet b = battery() > f;\n"
                                     "let g = i > 0 && b;\n");
    CHECK(program.lets[0].type.kind == TypeKind::Int);
    CHECK(program.lets[1].type.kind == TypeKind::Float);
    CHECK(HasError(Errors(CompileRobot("use robot;\nlet a = abs(true);\n")), 2, "abs takes numbers, got a bool"));
    CHECK(HasError(Errors(CompileRobot("use robot;\nlet a = min(1);\n")), 2, "min takes 2 values, got 1"));
}

TEST_CASE("Checker: no two things in a file share a name, and reserved words name nothing")
{
    CHECK(HasError(Errors(CompileRobot("use robot;\nparam x: float;\nconst x = 1;\n")), 3,
                   "\"x\" is already declared on line 2"));
    CHECK(HasError(Errors(CompileRobot("use robot;\nenum x { a }\nlet x = 1;\n")), 3, "already declared on line 2"));
    CHECK(HasError(Errors(CompileRobot("use robot;\nparam when: float;\n")), 2, "reserved word"));
    CHECK(HasError(Errors(CompileRobot("use robot;\nparam emit: float;\n")), 2, "word of the robot vocabulary"));
    CHECK(HasError(Errors(CompileRobot("use robot;\nparam abs: float;\n")), 2, "the name of a function"));
}

TEST_CASE("Checker: a let holds no strings, which only consts do")
{
    CHECK(HasError(Errors(CompileRobot("use robot;\nlet s = \"beep\";\n")), 2, "make \"s\" a const"));
}

TEST_CASE("Checker: what nothing uses is a warning, and the file still compiles")
{
    const Program program =
        Compiled("use robot;\nparam idle: float;\nenum Unused { a }\nconst k = 2;\nlet x = k > 1;\n");
    std::vector<std::string> messages;
    for (const Diagnostic &warning : program.warnings)
    {
        CHECK(warning.severity == Severity::Warning);
        messages.push_back(warning.message);
    }
    CHECK(messages == std::vector<std::string>{"param \"idle\" is never used", "enum \"Unused\" is never used",
                                               "let \"x\" is never used"});
}

TEST_CASE("Checker: a library holds only imports, enums and consts, and warns about nothing unused")
{
    const Program library = Compiled("use robot;\nsigiltype library;\nenum Mode { a }\nconst k = 2;\n");
    CHECK(library.library);
    CHECK(library.warnings.empty());

    const Diagnostics errors = Errors(CompileRobot(
        "use robot;\nsigiltype library;\nparam p: float;\nlet x = 1;\nmachine m { node a { } }\n"));
    CHECK(HasError(errors, 3, "not params"));
    CHECK(HasError(errors, 4, "not lets"));
    CHECK(HasError(errors, 5, "not blocks"));
    CHECK(HasError(Errors(CompileRobot("use robot;\nsigiltype libary;\n")), 2, "did you mean \"library\""));
}
