add_executable(Assisi-Net-Tests
  main.cpp
  TestNetTransport.cpp
)

Assisi_apply_test_defaults(Assisi-Net-Tests)

# doctest is header-only (DOWNLOAD_ONLY); pull it in as SYSTEM headers so its
# macros don't trip our strict warning set.
target_include_directories(Assisi-Net-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Net-Tests
  PRIVATE
    Assisi::Net
    Assisi::TestSupport
)

# These bind real loopback sockets and wait on a service thread, so they are
# slower than the pure-logic suites — and the simulated-lag case deliberately
# waits out 25 ms of induced latency per hop. Give the whole suite room without
# letting a genuine hang run forever.
add_test(NAME Net COMMAND Assisi-Net-Tests)
set_tests_properties(Net PROPERTIES TIMEOUT 120)

set_target_properties(Assisi-Net-Tests PROPERTIES FOLDER "tests")
