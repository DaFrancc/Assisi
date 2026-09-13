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
  TestBiMap.cpp
  TestBinaryCodec.cpp
  TestComponentMask.cpp
  TestBitStream.cpp
  TestComponentRegistry.cpp
  TestContentHash.cpp
  TestDiagnostics.cpp
  TestEventQueue.cpp
  TestFieldBounds.cpp
  TestJobSystem.cpp
  TestLogger.cpp
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

# Separate binary because its subject is "this registry has not been queried
# yet". Finalization is one-way and the registries are process singletons, so a
# single query from any other case — in whatever order doctest picks — makes
# every assertion in it vacuous. Same reason Assisi-Chiara-PreInit-Tests exists.
# Nothing linked here may query either registry outside that file.
add_executable(Assisi-Core-RegistryPreFinalize-Tests
  main.cpp
  TestRegistryPreFinalize.cpp
)

Assisi_apply_test_defaults(Assisi-Core-RegistryPreFinalize-Tests)

target_include_directories(Assisi-Core-RegistryPreFinalize-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Core-RegistryPreFinalize-Tests
  PRIVATE
    Assisi::Core
    Assisi::TestSupport
)

add_test(NAME Core-RegistryPreFinalize COMMAND Assisi-Core-RegistryPreFinalize-Tests)

set_target_properties(Assisi-Core-RegistryPreFinalize-Tests PROPERTIES FOLDER "tests")
