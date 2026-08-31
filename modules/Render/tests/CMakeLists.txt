add_executable(Assisi-Render-Tests
  main.cpp
  TestPostProcessChain.cpp
  TestShadowCascades.cpp
  TestShadowSettings.cpp
  TestShadowViews.cpp
  TestSky.cpp
  TestTonemap.cpp
)

Assisi_apply_test_defaults(Assisi-Render-Tests)

# doctest is header-only (DOWNLOAD_ONLY); pull it in as SYSTEM headers so its
# macros don't trip our strict warning set.
target_include_directories(Assisi-Render-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Render-Tests
  PRIVATE
    Assisi::Render
)

add_test(NAME Render COMMAND Assisi-Render-Tests)

set_target_properties(Assisi-Render-Tests PROPERTIES FOLDER "tests")
