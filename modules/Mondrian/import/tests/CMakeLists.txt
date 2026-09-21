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

# The markup parser and the screen compiler. Its own binary rather than cases in
# the one above: nothing it covers touches a font, and it links no rasteriser.
#
# It builds its own EventCatalog per case rather than linking the generated
# registrations, so what an `on_click` resolves against is what the case says it
# is and not whatever this binary happens to have linked.
add_executable(Assisi-MondrianImport-Tests
  main.cpp
  TestMarkup.cpp
  TestScreenCompiler.cpp
)

Assisi_apply_test_defaults(Assisi-MondrianImport-Tests)

target_include_directories(Assisi-MondrianImport-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-MondrianImport-Tests
  PRIVATE
    Assisi::MondrianImport
    Assisi::TestSupport
)

# The screens the game ships, read in place rather than copied into a fixture.
# Without these the first thing to notice a mistake in one would be a cook,
# which no test runs.
target_compile_definitions(Assisi-MondrianImport-Tests PRIVATE
  ASSISI_PAUSE_SCREEN_PATH="${CMAKE_SOURCE_DIR}/assets/ui/Pause.amdn"
  ASSISI_CONTROLS_SCREEN_PATH="${CMAKE_SOURCE_DIR}/assets/ui/Controls.amdn"
)

add_test(NAME MondrianImport COMMAND Assisi-MondrianImport-Tests)

set_target_properties(Assisi-MondrianImport-Tests PROPERTIES FOLDER "tests")
