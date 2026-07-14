add_executable(Assisi-Geometry-Tests
  main.cpp
  TestDefaultMeshes.cpp
  TestMeshImporter.cpp
)

Assisi_apply_defaults(Assisi-Geometry-Tests)

# doctest is header-only (DOWNLOAD_ONLY); pull it in as SYSTEM headers so its
# macros don't trip our strict warning set.
target_include_directories(Assisi-Geometry-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Geometry-Tests
  PRIVATE
    Assisi::Geometry
)

add_test(NAME Geometry COMMAND Assisi-Geometry-Tests)

set_target_properties(Assisi-Geometry-Tests PROPERTIES FOLDER "tests")
