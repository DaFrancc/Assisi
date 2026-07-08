add_executable(Assisi-ECS-Tests
  main.cpp
  TestSparseSet.cpp
  TestRegistry.cpp
  TestQuery.cpp
  TestScene.cpp
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
)

add_test(NAME ECS COMMAND Assisi-ECS-Tests)

set_target_properties(Assisi-ECS-Tests PROPERTIES FOLDER "tests")
