add_executable(Assisi-Sigil-Tests
  main.cpp
  BytecodeTesting.hpp
  SigilTesting.hpp
  TestBenchmark.cpp
  TestChecker.cpp
  TestDiagnostics.cpp
  TestEvaluate.cpp
  TestGraph.cpp
  TestImports.cpp
  TestIntegration.cpp
  TestLexer.cpp
  TestParser.cpp
  TestStateGraph.cpp
  TestSuggest.cpp
  TestTriggers.cpp
  TestVerify.cpp
  TestVocabulary.cpp
)

Assisi_apply_test_defaults(Assisi-Sigil-Tests)

target_include_directories(Assisi-Sigil-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Sigil-Tests
  PRIVATE
    Assisi::SigilCompile
)

# The integration test compiles the examples sglc ships, so they stay ones
# that compile and refuse as the book says.
target_compile_definitions(Assisi-Sigil-Tests PRIVATE
  ASSISI_SGLC_EXAMPLES="${PROJECT_SOURCE_DIR}/apps/sglc/examples"
)

add_test(NAME Sigil COMMAND Assisi-Sigil-Tests)

set_target_properties(Assisi-Sigil-Tests PROPERTIES FOLDER "tests")
