add_executable(Assisi-Geometry-Tests
  main.cpp
  TestBounds.cpp
  TestMeshBounds.cpp
  TestDefaultMeshes.cpp
  TestMeshImporter.cpp
  TestMaterialFile.cpp
  TestAssetImport.cpp
)

Assisi_apply_test_defaults(Assisi-Geometry-Tests)

# doctest is header-only (DOWNLOAD_ONLY); pull it in as SYSTEM headers so its
# macros don't trip our strict warning set.
target_include_directories(Assisi-Geometry-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Geometry-Tests
  PRIVATE
    Assisi::Geometry
)

# MaterialData's AssetTypeRegistry registration lives in the generated OBJECT
# library; force its TU into the test link (OBJECT libs are otherwise only
# pulled in on demand) so DeserializeMaterial/SerializeMaterial find the meta.
target_sources(Assisi-Geometry-Tests PRIVATE
  $<TARGET_OBJECTS:Assisi-Geometry-Generated>
)

add_test(NAME Geometry COMMAND Assisi-Geometry-Tests)

set_target_properties(Assisi-Geometry-Tests PROPERTIES FOLDER "tests")
