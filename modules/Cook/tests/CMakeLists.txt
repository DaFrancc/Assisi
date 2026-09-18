add_executable(Assisi-Cook-Tests
  main.cpp
  TestManifest.cpp
  TestCookTree.cpp
  TestPakWriter.cpp
)

Assisi_apply_test_defaults(Assisi-Cook-Tests)

target_include_directories(Assisi-Cook-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Cook-Tests
  PRIVATE
    Assisi::Cook
    Assisi::TestSupport
)

# The cook loads levels, which walks ComponentRegistry — populated by static
# initializers in the *-Generated OBJECT libraries. Listed explicitly rather than
# through assisi_link_reflections() so this binary does not drag in
# Physics-Generated, and Jolt with it, for a fixture that has no bodies.
#
# What that costs is real and the fixture is built around it: only the asset and
# component types these three declare exist here, so the fixture reflects a
# MaterialData (Geometry) and not an AppConfig (App). The cooker itself links
# every reflected type in the build — see apps/cook — so a config file cooks
# there through the same code path a .amat takes here.
target_sources(Assisi-Cook-Tests PRIVATE
  $<TARGET_OBJECTS:Assisi-ECS-Generated>
  $<TARGET_OBJECTS:Assisi-Runtime-Generated>
  $<TARGET_OBJECTS:Assisi-Geometry-Generated>
)

# Where the fixture asset tree lives, so a test can point an asset root at it
# without guessing the build layout.
target_compile_definitions(Assisi-Cook-Tests PRIVATE
  ASSISI_COOK_FIXTURE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

add_test(NAME Cook COMMAND Assisi-Cook-Tests)

set_target_properties(Assisi-Cook-Tests PROPERTIES FOLDER "tests")
