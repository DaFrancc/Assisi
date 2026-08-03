# Scene indexes pools by ComponentId, so a component the replication tests store
# must be reflected. Reflecting this INTERFACE lib (rather than the executable)
# lets the generated OBJECT library inherit the include dir and ECS dependency
# the registration code needs — the same shape modules/ECS/tests uses.
add_library(Assisi-NetSync-TestComponents INTERFACE)
target_include_directories(Assisi-NetSync-TestComponents INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/support/include"
)
# NetSync rather than ECS alone: the handler declarations take a NetContext,
# which is what makes them handlers rather than functions that happen to be
# named like them.
target_link_libraries(Assisi-NetSync-TestComponents INTERFACE Assisi::ECS Assisi::NetSync)
assisi_reflect(
  TARGET Assisi-NetSync-TestComponents
  HEADERS
    support/include/Assisi/NetSync/TestNetComponents.hpp
    support/include/Assisi/NetSync/TestMessageHandlers.hpp
)

add_executable(Assisi-NetSync-Tests
  main.cpp
  TestInputCommand.cpp
  TestNetClock.cpp
  TestNetSession.cpp
  TestBodyReplication.cpp
  TestComponentMaskCodec.cpp
  TestControl.cpp
  TestDistanceRelevancy.cpp
  TestInstancePolicy.cpp
  TestIntents.cpp
  TestMessages.cpp
  TestRelevancy.cpp
  TestReplication.cpp
)

Assisi_apply_test_defaults(Assisi-NetSync-Tests)

# doctest is header-only (DOWNLOAD_ONLY); pull it in as SYSTEM headers so its
# macros don't trip our strict warning set.
target_include_directories(Assisi-NetSync-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-NetSync-Tests
  PRIVATE
    Assisi::NetSync
    Assisi::TestSupport
    Assisi::Math
    Assisi-NetSync-TestComponents
)

# The replication tests exercise real reflected components (ECS::Transform, and
# the Replicated marker), so this executable needs their registration objects
# force-linked — an OBJECT library's translation units are otherwise only pulled
# in on demand, and a registration that nothing references by symbol is exactly
# the case that gets dropped.
target_sources(Assisi-NetSync-Tests PRIVATE
  $<TARGET_OBJECTS:Assisi-NetSync-Generated>
  $<TARGET_OBJECTS:Assisi-ECS-Generated>
  $<TARGET_OBJECTS:Assisi-Physics-Generated>
  $<TARGET_OBJECTS:Assisi-NetSync-TestComponents-Generated>
)

add_test(NAME NetSync COMMAND Assisi-NetSync-Tests)

set_target_properties(Assisi-NetSync-Tests PROPERTIES FOLDER "tests")
