# Scene indexes pools by Core::Reflect::ComponentId, so every component it
# stores must be reflected. These test-only components carry the include dir and
# ECS deps the generated registration code needs; reflecting this INTERFACE lib
# (rather than the executable) lets the generated OBJECT library inherit those
# usage requirements the way assisi_reflect expects.
add_library(Assisi-ECS-TestComponents INTERFACE)
target_include_directories(Assisi-ECS-TestComponents INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/support/include"
)
target_link_libraries(Assisi-ECS-TestComponents INTERFACE Assisi::ECS)
assisi_reflect(
  TARGET Assisi-ECS-TestComponents
  HEADERS
    support/include/Assisi/ECS/TestComponents.hpp
)

add_executable(Assisi-ECS-Tests
  main.cpp
  TestSparseSet.cpp
  TestRegistry.cpp
  TestQuery.cpp
  TestScene.cpp
  TestChangeDetection.cpp
)

Assisi_apply_defaults(Assisi-ECS-Tests)

# doctest is header-only (DOWNLOAD_ONLY); pull it in as SYSTEM headers so its
# macros don't trip our strict warning set.
target_include_directories(Assisi-ECS-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-ECS-Tests
  PRIVATE
    Assisi::ECS
    Assisi::TestSupport
    Assisi-ECS-TestComponents
)

# Force-include the reflected test components' registration objects (an OBJECT
# library's TUs are otherwise only pulled in on demand). Direct rather than via
# assisi_link_reflections so we take exactly these registrations, nothing else.
target_sources(Assisi-ECS-Tests PRIVATE
  $<TARGET_OBJECTS:Assisi-ECS-TestComponents-Generated>
)

add_test(NAME ECS COMMAND Assisi-ECS-Tests)

set_target_properties(Assisi-ECS-Tests PROPERTIES FOLDER "tests")
