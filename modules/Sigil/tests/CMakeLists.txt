add_executable(Assisi-Sigil-Tests
  main.cpp
  SigilTesting.hpp
  TestChecker.cpp
  TestDiagnostics.cpp
  TestGraph.cpp
  TestImports.cpp
  TestLexer.cpp
  TestParser.cpp
  TestSuggest.cpp
  TestTriggers.cpp
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

add_test(NAME Sigil COMMAND Assisi-Sigil-Tests)

set_target_properties(Assisi-Sigil-Tests PROPERTIES FOLDER "tests")
