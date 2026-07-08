add_executable(Assisi-Core-Tests
  main.cpp
  TestAssetSystem.cpp
  TestEventQueue.cpp
)

Assisi_apply_defaults(Assisi-Core-Tests)

target_include_directories(Assisi-Core-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Core-Tests
  PRIVATE
    Assisi::Core
)

add_test(NAME Core COMMAND Assisi-Core-Tests)

set_target_properties(Assisi-Core-Tests PROPERTIES FOLDER "tests")
