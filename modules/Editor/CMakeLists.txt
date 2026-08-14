add_library(Assisi-Editor)
Assisi_apply_defaults(Assisi-Editor)

# The editor tooling library (docs/editor-extraction-plan.md). A library, not
# an executable: component reflection registers per final binary, so the
# editor must be linked INTO the same executable as the game code it inspects
# (the Phase 2 GameEditor target). Nothing links this except editor
# executables — ship builds contain no editor code.

target_sources(Assisi-Editor
  PUBLIC
    "include/Assisi/Editor/ColliderPose.hpp"
    "include/Assisi/Editor/EditHistory.hpp"
    "include/Assisi/Editor/EditorApp.hpp"
    "include/Assisi/Editor/InspectorFieldChrome.hpp"
    "include/Assisi/Editor/PrePlayState.hpp"
  PRIVATE
    "src/ColliderPose.cpp"
    "src/EditHistory.cpp"
    "src/EditorApp.cpp"
    "src/EditorAssetBrowser.cpp"
    "src/EditorBlueprintMode.cpp"
    "src/EditorCamera.cpp"
    "src/EditorColliders.cpp"
    "src/EditorGizmo.cpp"
    "src/EditorInspector.cpp"
    "src/EditorLevels.cpp"
    "src/EditorOptions.cpp"
    "src/EditorOptionsPanel.hpp"
    "src/EditorPlay.cpp"
    "src/EditorWorlds.cpp"
    "src/GizmoDrag.cpp"
    "src/InspectorFieldChrome.cpp"
    "src/InstanceGesture.cpp"
    "src/PrePlayState.cpp"
    "src/ScenePick.cpp"
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
    Assisi::Runtime
    nlohmann_json::nlohmann_json
  PRIVATE
    # The transform gizmo (EditorGizmo.cpp) only; not part of the public API.
    imguizmo::imguizmo
)

# EditorNet.cpp is the multiplayer panel and the session plumbing — networking
# from the first line, so it leaves the build entirely rather than being wrapped
# in one enormous #if. Added with if()/target_sources rather than a $<BOOL:...>
# genex so the SOURCES property stays a list of real filenames (see the same note
# in modules/App/tests/CMakeLists.txt).
#
# The link is PUBLIC because EditorApp.hpp names NetSync types in its own
# interface — the session pointer and the level-identity accessors — so a
# consumer of the editor compiles against them too. Guarded by ASSISI_NETWORKING
# on both sides.
if (ASSISI_ENABLE_NETWORKING)
  target_sources(Assisi-Editor PRIVATE "src/EditorNet.cpp")
  target_link_libraries(Assisi-Editor PUBLIC Assisi::NetSync)
endif()

add_library(Assisi::Editor ALIAS Assisi-Editor)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
