/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestImports.cpp
/// @brief A file imports libraries: their own enums and consts arrive and
/// nothing else, names still don't collide, and a missing, broken, circular or
/// non-library import is refused where the import is written.

#include "SigilTesting.hpp"

#include <doctest/doctest.h>

using namespace Assisi::Sigil::Testing;

namespace
{

const Files kLibraries{
    {"movement.sgl", "use robot;\nsigiltype library;\nenum Mode { slow, fast }\nconst top = 3;\n"},
    {"wrapper.sgl", "use robot;\nsigiltype library;\nimport \"movement.sgl\";\nconst limit = top * 2;\n"},
    {"character.sgl", "use robot;\nparam p: bool;\n"},
    {"broken.sgl", "use robot;\nsigiltype library;\n\nconst k = nope;\n"},
    {"loop_a.sgl", "use robot;\nsigiltype library;\nimport \"loop_b.sgl\";\n"},
    {"loop_b.sgl", "use robot;\nsigiltype library;\nimport \"loop_a.sgl\";\n"},
    {"other.sgl", "use other;\nsigiltype library;\nconst k = 1;\n"},
};

} // namespace

TEST_CASE("Imports: a library's enums and consts can be used")
{
    std::expected<Program, Diagnostics> program =
        CompileRobot("use robot;\nimport \"movement.sgl\";\nparam m: Mode;\nlet fast = m == Mode.fast && top > 2;\n"
                     "machine x { node a { } node b { } a -> b when fast; }\n",
                     kLibraries);
    REQUIRE_MESSAGE(program.has_value(), Dump(Errors(program)));
    CHECK(program->warnings.empty());
    REQUIRE(program->enums.size() == 1);
    CHECK(program->enums[0].origin == "movement.sgl");
}

TEST_CASE("Imports: only a library's own names arrive, not those it imports")
{
    std::expected<Program, Diagnostics> program =
        CompileRobot("use robot;\nimport \"wrapper.sgl\";\nconst big = limit > 5;\nparam b: bool;\n"
                     "machine x { node a { } node c { } a -> c when b && big; }\n",
                     kLibraries);
    REQUIRE_MESSAGE(program.has_value(), Dump(Errors(program)));
    CHECK(HasError(Errors(CompileRobot("use robot;\nimport \"wrapper.sgl\";\nconst t = top;\n", kLibraries)), 3,
                   "unknown name \"top\""));
}

TEST_CASE("Imports: an imported name can't be declared again")
{
    CHECK(HasError(Errors(CompileRobot("use robot;\nimport \"movement.sgl\";\nconst top = 1;\n", kLibraries)), 3,
                   "\"top\" is already imported from \"movement.sgl\""));
}

TEST_CASE("Imports: an import that can't be used is refused where it is written")
{
    CHECK(HasError(Errors(CompileRobot("use robot;\nimport \"character.sgl\";\n", kLibraries)), 2,
                   "isn't a library"));
    CHECK(HasError(Errors(CompileRobot("use robot;\nimport \"missing.sgl\";\n", kLibraries)), 2,
                   "can't read \"missing.sgl\": no such file"));
    CHECK(HasError(Errors(CompileRobot("use robot;\nimport \"other.sgl\";\n", kLibraries)), 2,
                   "written for the other vocabulary, not robot"));
}

TEST_CASE("Imports: an error in an imported file is reported in that file")
{
    const Diagnostics errors = Errors(CompileRobot("use robot;\nimport \"broken.sgl\";\n", kLibraries));
    CHECK_MESSAGE(HasError(errors, 4, "unknown name \"nope\"", "broken.sgl"), Dump(errors));
    CHECK(HasError(errors, 2, "\"broken.sgl\" has errors"));
}

TEST_CASE("Imports: files that import each other are refused, not followed forever")
{
    const Diagnostics errors = Errors(CompileRobot("use robot;\nimport \"loop_a.sgl\";\n", kLibraries));
    CHECK_MESSAGE(HasError(errors, 3, "these files import each other", "loop_b.sgl"), Dump(errors));
}

TEST_CASE("Imports: an import nothing uses is a warning")
{
    std::expected<Program, Diagnostics> program =
        CompileRobot("use robot;\nimport \"movement.sgl\";\n", kLibraries);
    REQUIRE_MESSAGE(program.has_value(), Dump(Errors(program)));
    REQUIRE(program->warnings.size() == 1);
    CHECK(program->warnings[0].message == "nothing from \"movement.sgl\" is used");
}
