add_executable(Assisi-Window-Tests
  main.cpp
  TestActionMap.cpp
  TestInputBindings.cpp
)

Assisi_apply_test_defaults(Assisi-Window-Tests)

target_include_directories(Assisi-Window-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Window-Tests
  PRIVATE
    Assisi::Window
    nlohmann_json::nlohmann_json
)

# InputBindings' AssetTypeRegistry registration lives in the generated OBJECT
# library. Without its objects in the link nothing is registered, every document
# reads as NotRegistered, and the tests would be asserting against a loader that
# cannot see the type.
target_sources(Assisi-Window-Tests PRIVATE
  $<TARGET_OBJECTS:Assisi-Window-Generated>
)

add_test(NAME Window COMMAND Assisi-Window-Tests)

set_target_properties(Assisi-Window-Tests PROPERTIES FOLDER "tests")
