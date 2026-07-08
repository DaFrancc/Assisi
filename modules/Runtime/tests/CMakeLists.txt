add_executable(Assisi-Runtime-Tests
  main.cpp
  TestSceneSerializer.cpp
  TestHierarchy.cpp
)

Assisi_apply_defaults(Assisi-Runtime-Tests)

# doctest is header-only (DOWNLOAD_ONLY); pull it in as SYSTEM headers so its
# macros don't trip our strict warning set.
target_include_directories(Assisi-Runtime-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

target_link_libraries(Assisi-Runtime-Tests
  PRIVATE
    Assisi::Runtime
)

# The serializer walks ComponentRegistry, which is populated by the static
# initializers reflectgen emits into Assisi-Runtime-Generated. Those live in an
# OBJECT library, so pull its objects directly into the test link — otherwise no
# components are registered and Save/Load see an empty scene.
target_sources(Assisi-Runtime-Tests PRIVATE $<TARGET_OBJECTS:Assisi-Runtime-Generated>)

add_test(NAME Runtime COMMAND Assisi-Runtime-Tests)

set_target_properties(Assisi-Runtime-Tests PROPERTIES FOLDER "tests")
