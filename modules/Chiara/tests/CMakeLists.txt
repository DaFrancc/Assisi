# std::thread in the ring and concurrency suites. Chiara itself only links
# Threads when it is compiled in, but these tests spawn threads either way.
find_package(Threads REQUIRED)

add_executable(Assisi-Chiara-Tests
  main.cpp
  TestCapture.cpp
  TestEventRing.cpp
  TestMacros.cpp
  TestOverhead.cpp
  TestSerializer.cpp
  TestThreads.cpp
  TestTracks.cpp
)

Assisi_apply_test_defaults(Assisi-Chiara-Tests)

target_include_directories(Assisi-Chiara-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

# nlohmann is for *reading the trace back* in the serializer tests — asserting on
# a parsed document rather than on text. Chiara itself writes JSON by hand with
# std::format and links nothing, which is what keeps it below Core.
target_link_libraries(Assisi-Chiara-Tests
  PRIVATE
    Assisi::Chiara
    Threads::Threads
    nlohmann_json::nlohmann_json
)

add_test(NAME Chiara COMMAND Assisi-Chiara-Tests)

set_target_properties(Assisi-Chiara-Tests PROPERTIES FOLDER "tests")

# Separate binary because its subject is "Initialize has not been called yet",
# which cannot hold in a process where another case starts the capture — and
# doctest gives no ordering guarantee. Nothing linked here may call Initialize.
add_executable(Assisi-Chiara-PreInit-Tests
  main.cpp
  TestPreInit.cpp
)

Assisi_apply_test_defaults(Assisi-Chiara-PreInit-Tests)

target_include_directories(Assisi-Chiara-PreInit-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Chiara-PreInit-Tests
  PRIVATE
    Assisi::Chiara
    Threads::Threads
)

add_test(NAME Chiara-PreInit COMMAND Assisi-Chiara-PreInit-Tests)

set_target_properties(Assisi-Chiara-PreInit-Tests PROPERTIES FOLDER "tests")
