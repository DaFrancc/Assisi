# Systems the tests name from a level. Reflected on an INTERFACE lib rather than
# on the executable so the generated OBJECT library inherits the include dir and
# the App dependency the registration code needs — the same shape the NetSync
# tests use for their components.
add_library(Assisi-App-TestSystems INTERFACE)
target_include_directories(Assisi-App-TestSystems INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/support/include"
)
target_link_libraries(Assisi-App-TestSystems INTERFACE Assisi::App)
assisi_reflect(
  TARGET Assisi-App-TestSystems
  HEADERS
    support/include/Assisi/App/TestSystems.hpp
)

add_executable(Assisi-App-Tests
  main.cpp
  TestBlueprintLevel.cpp
  TestBlueprintVerbs.cpp
  TestBounce.cpp
  TestOscillate.cpp
  TestPerfCapture.cpp
  TestChildProcess.cpp
  TestInstanceViews.cpp
  TestLevelSystemsPrecheck.cpp
  TestSystemRegistry.cpp
  TestWorld.cpp
)

# Added with if() rather than a $<BOOL:...> generator expression on purpose.
# Assisi_apply_test_defaults reads the target's SOURCES property to attach
# per-source warning opt-outs, and an unevaluated genex comes back as its own
# literal text — it matches no file, so the opt-outs silently skip that source. A
# plain conditional keeps SOURCES a list of real filenames, which is what that
# property is being read as.
if (ASSISI_ENABLE_NETWORKING)
  target_sources(Assisi-App-Tests PRIVATE TestBlueprintReplication.cpp)
endif()

Assisi_apply_test_defaults(Assisi-App-Tests)

# The typed-view test spawns the *same* blueprints the build generated views
# from, so it has to reach the directory the generator read. Handed in rather
# than copied into the test: a second copy of the fixtures would drift, and the
# whole point of the test is that the generated names and the runtime names
# cannot drift.
target_compile_definitions(Assisi-App-Tests PRIVATE
  ASSISI_INSTANCE_VIEW_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/blueprints"
)
# The generated header this includes is ordered by Assisi::App, which the
# generator target is attached to — stating it again here would be a forward
# reference, because this file is configured before the top-level call runs.

target_include_directories(Assisi-App-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

# LogCapture.hpp, for the tests here whose subject is a log line. Reached in the
# Runtime tests rather than copied for the reason stated above about fixtures: a
# second copy drifts, and both copies claim to be the one the logger installs.
target_include_directories(Assisi-App-Tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../Runtime/tests"
)

target_link_libraries(Assisi-App-Tests
  PRIVATE
    Assisi::App
    Assisi-App-TestSystems
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
  $<TARGET_OBJECTS:Assisi-App-TestSystems-Generated>
  # Replicated is an ACOMP in NetSync; without its registrations the loader has
  # no ComponentId for it and a blueprint declaring one silently gets an entity
  # that never replicates.
  $<$<BOOL:${ASSISI_ENABLE_NETWORKING}>:$<TARGET_OBJECTS:Assisi-NetSync-Generated>>
)

add_test(NAME App COMMAND Assisi-App-Tests)

set_target_properties(Assisi-App-Tests PROPERTIES FOLDER "tests")
