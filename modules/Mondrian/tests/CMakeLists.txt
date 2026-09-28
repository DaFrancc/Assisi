# Structs reflected only for the tests. Reflected on an INTERFACE lib rather than
# on the executable so the generated OBJECT library inherits the include dir and
# the Mondrian dependency its code needs.
add_library(Assisi-Mondrian-TestStructs INTERFACE)
target_include_directories(Assisi-Mondrian-TestStructs INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/support/include"
)
target_link_libraries(Assisi-Mondrian-TestStructs INTERFACE Assisi::Mondrian)
assisi_reflect(
  TARGET Assisi-Mondrian-TestStructs
  HEADERS
    support/include/Assisi/Mondrian/TestReflectedStructs.hpp
)

add_executable(Assisi-Mondrian-Tests
  main.cpp
  TestReflectedStructs.cpp
  TestDraw.cpp
  TestDrawList.cpp
  TestFont.cpp
  TestHitTest.cpp
  TestLayout.cpp
  TestNavigation.cpp
  TestNodeTree.cpp
  TestPattern.cpp
  TestScreenBlob.cpp
  TestScreenLoader.cpp
  TestScreens.cpp
  TestText.cpp
  TestTextField.cpp
  TestUi.cpp
  TestUtf8.cpp
  TestWidget.cpp
  TestWidgets.cpp
)

Assisi_apply_test_defaults(Assisi-Mondrian-Tests)

# doctest is header-only (DOWNLOAD_ONLY); pull it in as SYSTEM headers so its
# macros don't trip our strict warning set.
target_include_directories(Assisi-Mondrian-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

# The core only: these tests are the proof that it runs without a GPU or window.
target_link_libraries(Assisi-Mondrian-Tests
  PRIVATE
    Assisi::Mondrian
    Assisi::TestSupport
    Assisi-Mondrian-TestStructs
)

# The screen document's generated table, which the blob reader and writer walk,
# and the test-only structs'.
target_sources(Assisi-Mondrian-Tests PRIVATE
  $<TARGET_OBJECTS:Assisi-Mondrian-Generated>
  $<TARGET_OBJECTS:Assisi-Mondrian-TestStructs-Generated>
)

add_test(NAME Mondrian COMMAND Assisi-Mondrian-Tests)

set_target_properties(Assisi-Mondrian-Tests PROPERTIES FOLDER "tests")
