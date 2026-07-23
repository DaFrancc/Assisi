add_library(Assisi-Editor)
Assisi_apply_defaults(Assisi-Editor)

# The editor tooling library (docs/editor-extraction-plan.md). A library, not
# an executable: component reflection registers per final binary, so the
# editor must be linked INTO the same executable as the game code it inspects
# (the Phase 2 GameEditor target). Nothing links this except editor
# executables — ship builds contain no editor code.

target_sources(Assisi-Editor
  PUBLIC
    "include/Assisi/Editor/EditHistory.hpp"
  PRIVATE
    "src/EditHistory.cpp"
)

target_include_directories(Assisi-Editor
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Editor
  PUBLIC
    # EditHistory's real dependencies. E2 (EditorApp + panels) grows this to
    # Assisi::App, imguizmo, implot.
    Assisi::Runtime
    nlohmann_json::nlohmann_json
)

add_library(Assisi::Editor ALIAS Assisi-Editor)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
