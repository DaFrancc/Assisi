add_executable(Assisi-FontImport-Tests
  main.cpp
  TestFontImport.cpp
)

Assisi_apply_test_defaults(Assisi-FontImport-Tests)

target_include_directories(Assisi-FontImport-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

# FontDescription's registration, which parsing a document looks the type up by.
target_sources(Assisi-FontImport-Tests PRIVATE
  $<TARGET_OBJECTS:Assisi-Mondrian-Generated>
)

target_link_libraries(Assisi-FontImport-Tests
  PRIVATE
    Assisi::FontImport
    Assisi::TestSupport
)

# The font the game ships, read in place rather than copied into a fixture.
target_compile_definitions(Assisi-FontImport-Tests PRIVATE
  ASSISI_TEST_FONT_PATH="${CMAKE_SOURCE_DIR}/assets/fonts/Inter-Regular.ttf"
)

add_test(NAME FontImport COMMAND Assisi-FontImport-Tests)

set_target_properties(Assisi-FontImport-Tests PROPERTIES FOLDER "tests")
