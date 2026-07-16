# Editor-only unit tests. The sandbox is an executable, not a library, so the
# handful of editor subsystems that are worth testing in isolation (EditHistory)
# are compiled straight into this test target from ../src rather than linked.
add_executable(Assisi-Sandbox-Tests
  main.cpp
  TestEditHistory.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/EditHistory.cpp"
)

Assisi_apply_defaults(Assisi-Sandbox-Tests)

# doctest is header-only; pull it in as SYSTEM headers so its macros don't trip
# our strict warning set.
target_include_directories(Assisi-Sandbox-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

# So the test can include the editor sources by their bare name ("EditHistory.hpp").
target_include_directories(Assisi-Sandbox-Tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)

target_link_libraries(Assisi-Sandbox-Tests
  PRIVATE
    Assisi::Runtime
    nlohmann_json::nlohmann_json
)

# EditHistory walks ComponentRegistry, populated by the static initializers
# reflectgen emits into the *-Generated OBJECT libraries. Pull ECS + Runtime
# generated objects in so Transform / Parent / MeshRenderer / Camera register;
# listed explicitly (not via assisi_link_reflections) to avoid dragging in
# Physics-Generated + Jolt, which these tests don't exercise.
target_sources(Assisi-Sandbox-Tests PRIVATE
  $<TARGET_OBJECTS:Assisi-ECS-Generated>
  $<TARGET_OBJECTS:Assisi-Runtime-Generated>
)

add_test(NAME Sandbox COMMAND Assisi-Sandbox-Tests)

set_target_properties(Assisi-Sandbox-Tests PROPERTIES FOLDER "tests")
