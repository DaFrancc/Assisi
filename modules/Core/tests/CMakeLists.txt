# Header-only helpers shared by every module's test suite (e.g. the
# ThrowOnContractViolation guard for exercising ASSISI_ASSERT paths).
add_library(Assisi-TestSupport INTERFACE)
target_include_directories(Assisi-TestSupport INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/support"
)
target_link_libraries(Assisi-TestSupport INTERFACE Assisi::Core)
add_library(Assisi::TestSupport ALIAS Assisi-TestSupport)

# Two asset kinds only tests have, registered from a source file compiled into
# each test executable that links this, the way a module registers its own.
add_library(Assisi-TestAssetKinds INTERFACE)
target_sources(Assisi-TestAssetKinds INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/support/Assisi/Testing/TestAssetKinds.cpp"
)
target_link_libraries(Assisi-TestAssetKinds INTERFACE Assisi::TestSupport)
add_library(Assisi::TestAssetKinds ALIAS Assisi-TestAssetKinds)

# A message only these tests declare, reflected the way a module reflects its
# own, so receiving one is tested without the network.
add_library(Assisi-Core-TestMessages INTERFACE)
target_link_libraries(Assisi-Core-TestMessages INTERFACE Assisi::TestSupport)
assisi_reflect(
  TARGET Assisi-Core-TestMessages
  INCLUDE_ROOT support
  HEADERS
    support/Assisi/Testing/TestMessages.hpp
)

add_executable(Assisi-Core-Tests
  main.cpp
  TestAssetDatabase.cpp
  TestAssetId.cpp
  TestAssetIdJson.cpp
  TestAssetIgnore.cpp
  TestAssetKind.cpp
  TestAssetPath.cpp
  TestAssetStore.cpp
  TestAssetSystem.cpp
  TestBiMap.cpp
  TestAssetCodec.cpp
  TestBinaryCodec.cpp
  TestBitmask.cpp
  TestBitmaskJson.cpp
  TestEnumLabels.cpp
  TestComponentMask.cpp
  TestBitStream.cpp
  TestComponentRegistry.cpp
  TestContainerCodec.cpp
  TestContainerOps.cpp
  TestContainerJson.cpp
  TestContentHash.cpp
  TestCookedBlob.cpp
  TestCookedPayload.cpp
  TestDiagnostics.cpp
  TestDisplayedString.cpp
  TestEventCatalog.cpp
  TestEventQueue.cpp
  TestEventRoute.cpp
  TestFieldBounds.cpp
  TestInternedString.cpp
  TestJobSystem.cpp
  TestPakCodec.cpp
  TestPredictedCompare.cpp
  TestReceived.cpp
  TestPakFormat.cpp
  TestPakProvider.cpp
  TestRandomAccessFile.cpp
  TestLogger.cpp
  TestStringCodec.cpp
  TestStringPool.cpp
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
    Assisi::TestAssetKinds
    Assisi-Core-TestMessages
)

target_sources(Assisi-Core-Tests PRIVATE $<TARGET_OBJECTS:Assisi-Core-TestMessages-Generated>)

Assisi_add_doctest_tests(Assisi-Core-Tests NAME Core)

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

Assisi_add_doctest_tests(Assisi-Core-RegistryPreFinalize-Tests NAME Core-RegistryPreFinalize)

set_target_properties(Assisi-Core-RegistryPreFinalize-Tests PROPERTIES FOLDER "tests")
