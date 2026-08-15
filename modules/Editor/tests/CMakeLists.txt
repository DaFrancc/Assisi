# Editor unit tests. EditHistory is deliberately light (Core/ECS/Runtime +
# nlohmann only), but the Editor *library* carries the whole App stack (ImGui,
# GLFW, Jolt via Assisi::App) — so the suite compiles the sources it tests
# directly rather than linking Assisi::Editor, keeping the test binary free of
# windowing/physics baggage.
add_executable(Assisi-Editor-Tests
  main.cpp
  TestColliderPose.cpp
  TestEditHistory.cpp
  TestEntityPicking.cpp
  TestGizmoDrag.cpp
  TestInspectorFieldChrome.cpp
  TestInstanceGesture.cpp
  TestInstanceHistory.cpp
  TestPrePlayState.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/ColliderPose.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/EditHistory.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/GizmoDrag.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/InspectorFieldChrome.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/InstanceGesture.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/PrePlayState.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/ScenePick.cpp"
)

Assisi_apply_test_defaults(Assisi-Editor-Tests)

# GCC-only, optimized builds only: constructing a ComponentDelta (which holds two
# std::optional<nlohmann::json>) and moving it into the command variant makes GCC
# report a maybe-uninitialized read *inside nlohmann's move constructor*. Every
# member is explicitly initialized at the call site and restructuring the call
# does not shift it — it is a known false positive from inlining optional<json>
# through a variant. Scoped to this one file so the rest of the suite keeps the
# check.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set_source_files_properties(TestEditHistory.cpp
    TARGET_DIRECTORY Assisi-Editor-Tests
    PROPERTIES COMPILE_OPTIONS -Wno-maybe-uninitialized)
endif()

# doctest is header-only; pull it in as SYSTEM headers so its macros don't trip
# our strict warning set.
target_include_directories(Assisi-Editor-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_include_directories(Assisi-Editor-Tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
)

target_link_libraries(Assisi-Editor-Tests
  PRIVATE
    Assisi::Runtime
    nlohmann_json::nlohmann_json
    # InspectorFieldChrome scopes an ImGui colour push, so the suite needs core
    # ImGui. Core only — the backends live in Assisi::App, which stays out.
    imgui::imgui
)

# EditHistory walks ComponentRegistry, populated by the static initializers
# reflectgen emits into the *-Generated OBJECT libraries. Pull ECS + Runtime
# generated objects in so Transform / Parent / MeshRenderer / Camera register;
# listed explicitly (not via assisi_link_reflections) to avoid dragging in
# Physics-Generated + Jolt, which these tests don't exercise.
target_sources(Assisi-Editor-Tests PRIVATE
  $<TARGET_OBJECTS:Assisi-ECS-Generated>
  $<TARGET_OBJECTS:Assisi-Runtime-Generated>
)

add_test(NAME Editor COMMAND Assisi-Editor-Tests)

set_target_properties(Assisi-Editor-Tests PROPERTIES FOLDER "tests")
