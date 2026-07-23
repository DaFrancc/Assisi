add_executable(Assisi-NetSync-Tests
  main.cpp
  TestInputCommand.cpp
  TestNetClock.cpp
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
)

# The replication tests exercise real reflected components (ECS::Transform, and
# the Replicated marker), so this executable needs their registration objects
# force-linked — an OBJECT library's translation units are otherwise only pulled
# in on demand, and a registration that nothing references by symbol is exactly
# the case that gets dropped.
target_sources(Assisi-NetSync-Tests PRIVATE
  $<TARGET_OBJECTS:Assisi-NetSync-Generated>
  $<TARGET_OBJECTS:Assisi-ECS-Generated>
)

add_test(NAME NetSync COMMAND Assisi-NetSync-Tests)

set_target_properties(Assisi-NetSync-Tests PROPERTIES FOLDER "tests")
