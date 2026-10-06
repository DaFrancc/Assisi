# Physics unit tests. Unlike the Editor suite these link the real Assisi::Physics
# (and thus Jolt): the behaviour under test is the writeback from a stepped Jolt
# simulation into ECS Transforms, which cannot be exercised without a live world.
add_executable(Assisi-Physics-Tests
  main.cpp
  TestBodyLifetime.cpp
  TestBodyState.cpp
  TestCharacter.cpp
  TestCharacterFeel.cpp
  TestCollisionAssets.cpp
  TestCollisionChannels.cpp
  TestCompoundBodies.cpp
  TestContactEvents.cpp
  TestFollowers.cpp
  TestGameplayRequests.cpp
  TestJoints.cpp
  TestNonFinite.cpp
  TestParentedBodies.cpp
  TestPhysicsWriteback.cpp
  TestRigidBody.cpp
  TestTransformPush.cpp
  TestTriggers.cpp
  TestWorldQueries.cpp
)

Assisi_apply_test_defaults(Assisi-Physics-Tests)

# doctest is header-only (DOWNLOAD_ONLY); pull it in as SYSTEM headers so its
# macros don't trip our strict warning set.
target_include_directories(Assisi-Physics-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Physics-Tests
  PRIVATE
    Assisi::Physics
)

# LogCapture.hpp, for the tests whose subject is an error naming an entity.
# Reached in the Runtime tests rather than copied, as the Geometry tests do.
target_include_directories(Assisi-Physics-Tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../Runtime/tests"
)

# Scene stores components by ComponentId, which the static initializers reflectgen
# emits into the *-Generated OBJECT libraries assign. Transform lives in the ECS
# layer (Assisi-ECS-Generated); Collider / RigidBody in Physics's own.
# Without both, the pools these tests touch have no registration and Add<T> trips
# its debug contract assert (and stores nothing).
target_sources(Assisi-Physics-Tests PRIVATE
  $<TARGET_OBJECTS:Assisi-ECS-Generated>
  $<TARGET_OBJECTS:Assisi-Physics-Generated>
)

add_test(NAME Physics COMMAND Assisi-Physics-Tests)

set_target_properties(Assisi-Physics-Tests PROPERTIES FOLDER "tests")
