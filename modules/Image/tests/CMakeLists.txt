add_executable(Assisi-Image-Tests
  main.cpp
  TestImageLayout.cpp
  TestCompress.cpp
  TestCookedTexture.cpp
)

Assisi_apply_test_defaults(Assisi-Image-Tests)

target_include_directories(Assisi-Image-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Image-Tests
  PRIVATE
    Assisi::ImageImport
    Assisi::TestSupport
)

Assisi_add_doctest_tests(Assisi-Image-Tests NAME Image)

set_target_properties(Assisi-Image-Tests PROPERTIES FOLDER "tests")
