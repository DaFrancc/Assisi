add_executable(Assisi-Mondrian-Tests
  main.cpp
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
)

add_test(NAME Mondrian COMMAND Assisi-Mondrian-Tests)

set_target_properties(Assisi-Mondrian-Tests PROPERTIES FOLDER "tests")
