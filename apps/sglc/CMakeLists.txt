# sglc: the Sigil compiler on the command line. Compiles `.sgl` files and prints
# what the compiler says about them, so a file can be checked without a cook.
add_executable(Assisi-Sglc)
Assisi_apply_defaults(Assisi-Sglc)

target_sources(Assisi-Sglc PRIVATE
  "src/SglcMain.cpp"
  "src/Vocabularies.cpp"
  "src/Vocabularies.hpp"
)

target_link_libraries(Assisi-Sglc
  PRIVATE
    Assisi::SigilCompile
)

set_target_properties(Assisi-Sglc PROPERTIES OUTPUT_NAME "sglc")

if (ASSISI_BUILD_TESTS)
  # The examples the book points at: the good one must compile, and the broken
  # one must be refused for its own mistakes, not for a file it couldn't read.
  set(_sglc_examples "${CMAKE_CURRENT_SOURCE_DIR}/examples")
  add_test(NAME SglcCompilesExample
    COMMAND Assisi-Sglc --root "${_sglc_examples}" --color=never "${_sglc_examples}/arm.sgl")
  add_test(NAME SglcRefusesBrokenExample
    COMMAND Assisi-Sglc --root "${_sglc_examples}" --color=never "${_sglc_examples}/broken.sgl")
  set_tests_properties(SglcRefusesBrokenExample PROPERTIES PASS_REGULAR_EXPRESSION "broken.sgl: 6 errors")
endif()
