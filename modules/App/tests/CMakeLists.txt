add_executable(Assisi-App-Tests
  main.cpp
  TestBounce.cpp
  TestSystemRegistry.cpp
  TestWorld.cpp
)

Assisi_apply_test_defaults(Assisi-App-Tests)

target_include_directories(Assisi-App-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-App-Tests
  PRIVATE
    Assisi::App
)

# Scene indexes pools by ComponentId, which the static initializers reflectgen
# emits into the *-Generated OBJECT libraries populate. Without their objects in
# the link, Transform/RigidBody have no id and Scene::Add asserts. TestWorld
# steps real physics, so Physics-Generated is wanted here (unlike the Runtime
# tests, which deliberately avoid dragging in Jolt).
target_sources(Assisi-App-Tests PRIVATE
  $<TARGET_OBJECTS:Assisi-ECS-Generated>
  $<TARGET_OBJECTS:Assisi-Runtime-Generated>
  $<TARGET_OBJECTS:Assisi-Physics-Generated>
)

add_test(NAME App COMMAND Assisi-App-Tests)

set_target_properties(Assisi-App-Tests PROPERTIES FOLDER "tests")
