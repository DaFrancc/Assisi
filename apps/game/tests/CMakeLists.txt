# Tests of the sample game's own code, the first-person template above all: it
# is game code, so the engine's suites cannot reach it, and a template nobody
# runs stops working without anyone noticing.
add_executable(Assisi-Game-Tests
  main.cpp
  TestFirstPersonLook.cpp
  TestFirstPersonMovement.cpp
  TestFirstPersonView.cpp
  TestTemplateAssets.cpp
)

Assisi_apply_test_defaults(Assisi-Game-Tests)

target_include_directories(Assisi-Game-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Game-Tests
  PRIVATE
    Assisi-GameLib
    Assisi-FirstPerson-Systems
)

# Where the template keeps its assets and where the sample game keeps its
# copies, so the drift test compares the files themselves rather than a list
# someone has to keep.
target_compile_definitions(Assisi-Game-Tests PRIVATE
  ASSISI_TEMPLATE_ASSETS="${CMAKE_SOURCE_DIR}/templates/first-person/assets"
  ASSISI_GAME_ASSETS="${CMAKE_SOURCE_DIR}/assets"
)

# Every component the template declares needs its registration in the link,
# as in the game's own binaries.
assisi_link_reflections(Assisi-Game-Tests)

Assisi_add_doctest_tests(Assisi-Game-Tests NAME Game)

set_target_properties(Assisi-Game-Tests PROPERTIES FOLDER "tests")
