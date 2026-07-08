add_executable(Assisi-Window-Tests
  main.cpp
  TestActionMap.cpp
)

Assisi_apply_defaults(Assisi-Window-Tests)

target_include_directories(Assisi-Window-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Window-Tests
  PRIVATE
    Assisi::Window
    nlohmann_json::nlohmann_json
)

add_test(NAME Window COMMAND Assisi-Window-Tests)

set_target_properties(Assisi-Window-Tests PROPERTIES FOLDER "tests")
