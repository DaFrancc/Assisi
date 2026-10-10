add_executable(Assisi-Steam-Tests
  main.cpp
  TestAddToStat.cpp
  TestUnavailable.cpp
)

# Starting Steam for real is not tested here: it would reach whatever Steam
# client is running on the machine. Only the build without the SDK, whose answer
# never depends on one, has its Connect tested.
if (NOT ASSISI_STEAMWORKS_ENABLED)
  target_sources(Assisi-Steam-Tests PRIVATE TestConnectStub.cpp)
endif()

Assisi_apply_test_defaults(Assisi-Steam-Tests)

target_include_directories(Assisi-Steam-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Steam-Tests
  PRIVATE
    Assisi::Steam
)

add_test(NAME Steam COMMAND Assisi-Steam-Tests)

set_target_properties(Assisi-Steam-Tests PROPERTIES FOLDER "tests")
