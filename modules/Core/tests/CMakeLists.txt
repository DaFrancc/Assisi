# Header-only helpers shared by every module's test suite (e.g. the
# ThrowOnContractViolation guard for exercising ASSISI_ASSERT paths).
add_library(Assisi-TestSupport INTERFACE)
target_include_directories(Assisi-TestSupport INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/support"
)
target_link_libraries(Assisi-TestSupport INTERFACE Assisi::Core)
add_library(Assisi::TestSupport ALIAS Assisi-TestSupport)

add_executable(Assisi-Core-Tests
  main.cpp
  TestAssetDatabase.cpp
  TestAssetId.cpp
  TestAssetIdJson.cpp
  TestAssetPath.cpp
  TestAssetSystem.cpp
  TestBinaryCodec.cpp
  TestComponentMask.cpp
  TestBitStream.cpp
  TestComponentRegistry.cpp
  TestContentHash.cpp
  TestEventQueue.cpp
  TestJobSystem.cpp
  TestTrivialString.cpp
)

Assisi_apply_test_defaults(Assisi-Core-Tests)

target_include_directories(Assisi-Core-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Core-Tests
  PRIVATE
    Assisi::Core
    Assisi::TestSupport
)

add_test(NAME Core COMMAND Assisi-Core-Tests)

set_target_properties(Assisi-Core-Tests PROPERTIES FOLDER "tests")
