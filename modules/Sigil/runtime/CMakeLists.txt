# The Sigil runtime: the bytecode a compiled expression becomes, the check a
# cooked asset's code passes when it loads, and the evaluator that runs it.
# Depends on nothing but the standard library, so the game can link it.
add_library(Assisi-Sigil)
Assisi_apply_defaults(Assisi-Sigil)

target_sources(Assisi-Sigil
  PUBLIC
    "include/Assisi/Sigil/Bytecode.hpp"
    "include/Assisi/Sigil/Evaluate.hpp"
    "include/Assisi/Sigil/Graph.hpp"
    "include/Assisi/Sigil/Layout.hpp"
    "include/Assisi/Sigil/Verify.hpp"
  PRIVATE
    "src/Bytecode.cpp"
    "src/Evaluate.cpp"
    "src/Graph.cpp"
    "src/Layout.cpp"
    "src/Verify.cpp"
)

target_include_directories(Assisi-Sigil
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_library(Assisi::Sigil ALIAS Assisi-Sigil)
