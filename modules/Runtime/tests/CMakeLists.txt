add_executable(Assisi-Runtime-Tests
  main.cpp
  TestBlueprintExpansion.cpp
  TestBlueprintOverrides.cpp
  TestDrawItem.cpp
  TestMeshCuller.cpp
  TestFrustum.cpp
  TestSceneSerializer.cpp
  TestHierarchy.cpp
  TestLifecycle.cpp
  TestLightingSystem.cpp
  TestWireGating.cpp
)

Assisi_apply_test_defaults(Assisi-Runtime-Tests)

# doctest is header-only (DOWNLOAD_ONLY); pull it in as SYSTEM headers so its
# macros don't trip our strict warning set.
target_include_directories(Assisi-Runtime-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Runtime-Tests
  PRIVATE
    Assisi::Runtime
)

# The serializer walks ComponentRegistry, which is populated by the static
# initializers reflectgen emits into the *-Generated OBJECT libraries, so pull
# their objects directly into the test link — otherwise no components are
# registered and Save/Load see an empty scene. Transform lives in the ECS layer
# (Assisi-ECS-Generated); the render-facing components in Assisi-Runtime-Generated.
# Listed explicitly rather than via assisi_link_reflections() so we don't drag in
# Physics-Generated (and thus Jolt) that this test doesn't otherwise link.
target_sources(Assisi-Runtime-Tests PRIVATE
  $<TARGET_OBJECTS:Assisi-ECS-Generated>
  $<TARGET_OBJECTS:Assisi-Runtime-Generated>
)

add_test(NAME Runtime COMMAND Assisi-Runtime-Tests)

set_target_properties(Assisi-Runtime-Tests PROPERTIES FOLDER "tests")
