add_executable(Assisi-NetSync-Tests
  main.cpp
  TestInputCommand.cpp
  TestNetClock.cpp
)

Assisi_apply_test_defaults(Assisi-NetSync-Tests)

# doctest is header-only (DOWNLOAD_ONLY); pull it in as SYSTEM headers so its
# macros don't trip our strict warning set.
target_include_directories(Assisi-NetSync-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-NetSync-Tests
  PRIVATE
    Assisi::NetSync
    Assisi::TestSupport
)

add_test(NAME NetSync COMMAND Assisi-NetSync-Tests)

set_target_properties(Assisi-NetSync-Tests PROPERTIES FOLDER "tests")
