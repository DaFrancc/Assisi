add_executable(Assisi-App-Tests
  main.cpp
  TestSystemRegistry.cpp
)

Assisi_apply_test_defaults(Assisi-App-Tests)

target_include_directories(Assisi-App-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-App-Tests
  PRIVATE
    Assisi::App
)

add_test(NAME App COMMAND Assisi-App-Tests)

set_target_properties(Assisi-App-Tests PROPERTIES FOLDER "tests")
