add_executable(Assisi-Image-Tests
  main.cpp
  TestImageLayout.cpp
  TestCompress.cpp
)

Assisi_apply_test_defaults(Assisi-Image-Tests)

target_include_directories(Assisi-Image-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Image-Tests
  PRIVATE
    Assisi::Image
    Assisi::TestSupport
)

add_test(NAME Image COMMAND Assisi-Image-Tests)

set_target_properties(Assisi-Image-Tests PROPERTIES FOLDER "tests")
