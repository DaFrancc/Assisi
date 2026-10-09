# The Sigil compiler: lexer, parser and checker, turning a `.sgl` file into a
# checked program against a vocabulary. The cook and the editor link it; the
# game must not, and its link guard says so.
add_library(Assisi-SigilCompile)
Assisi_apply_defaults(Assisi-SigilCompile)

target_sources(Assisi-SigilCompile
  PUBLIC
    "include/Assisi/Sigil/Compile/Compile.hpp"
    "include/Assisi/Sigil/Compile/Diagnostic.hpp"
    "include/Assisi/Sigil/Compile/Lexer.hpp"
    "include/Assisi/Sigil/Compile/Parser.hpp"
    "include/Assisi/Sigil/Compile/Program.hpp"
    "include/Assisi/Sigil/Compile/Suggest.hpp"
    "include/Assisi/Sigil/Compile/Syntax.hpp"
    "include/Assisi/Sigil/Compile/Vocabulary.hpp"
  PRIVATE
    "src/CheckBlocks.cpp"
    "src/CheckDeclarations.cpp"
    "src/CheckExpressions.cpp"
    "src/Checker.hpp"
    "src/Compile.cpp"
    "src/Diagnostic.cpp"
    "src/Evaluate.cpp"
    "src/Lexer.cpp"
    "src/Parser.cpp"
    "src/Suggest.cpp"
    "src/Vocabulary.cpp"
)

target_include_directories(Assisi-SigilCompile
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_library(Assisi::SigilCompile ALIAS Assisi-SigilCompile)
