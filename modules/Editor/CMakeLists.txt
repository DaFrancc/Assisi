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
    "include/Assisi/Editor/EditorApp.hpp"
    "include/Assisi/Editor/PrePlayState.hpp"
  PRIVATE
    "src/EditHistory.cpp"
    "src/EditorApp.cpp"
    "src/EditorAssetBrowser.cpp"
    "src/EditorBlueprintMode.cpp"
    "src/EditorCamera.cpp"
    "src/EditorColliders.cpp"
    "src/EditorGizmo.cpp"
    "src/EditorInspector.cpp"
    "src/EditorLevels.cpp"
    "src/EditorNet.cpp"
    "src/EditorOptions.cpp"
    "src/EditorPlay.cpp"
    "src/EditorWorlds.cpp"
    "src/InstanceGesture.cpp"
    "src/PrePlayState.cpp"
    "src/ImGuiQueries.hpp"
)

target_include_directories(Assisi-Editor
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Editor
  PUBLIC
    # EditorApp derives from App::Application (its header needs App), and App
    # publicly brings the rest of what the panels touch: Debug (imgui/implot),
    # Render, Physics, Window, ECS, Core. Runtime is also a direct dependency
    # of EditHistory, kept explicit.
    Assisi::App
    Assisi::NetSync
    Assisi::Runtime
    nlohmann_json::nlohmann_json
  PRIVATE
    # The transform gizmo (EditorGizmo.cpp) only; not part of the public API.
    imguizmo::imguizmo
)

add_library(Assisi::Editor ALIAS Assisi-Editor)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
