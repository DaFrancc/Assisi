add_executable(Assisi-Math-Tests
  main.cpp
  TestAngles.cpp
)

Assisi_apply_test_defaults(Assisi-Math-Tests)

# doctest is header-only (DOWNLOAD_ONLY); pull it in as SYSTEM headers so its
# macros don't trip our strict warning set.
target_include_directories(Assisi-Math-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Math-Tests
  PRIVATE
    Assisi::Math
)

add_test(NAME Math COMMAND Assisi-Math-Tests)

set_target_properties(Assisi-Math-Tests PROPERTIES FOLDER "tests")
