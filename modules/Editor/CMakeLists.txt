add_library(Assisi-Editor)
Assisi_apply_defaults(Assisi-Editor)

# The editor tooling library. A library, not
# an executable: component reflection registers per final binary, so the
# editor must be linked INTO the same executable as the game code it inspects
# (the Phase 2 GameEditor target). Nothing links this except editor
# executables — ship builds contain no editor code.

target_sources(Assisi-Editor
  PUBLIC
    "include/Assisi/Editor/ColliderPose.hpp"
    "include/Assisi/Editor/EditHistory.hpp"
    "include/Assisi/Editor/EditorApp.hpp"
    "include/Assisi/Editor/EditorChiaraPanel.hpp"
    "include/Assisi/Editor/InspectorFieldChrome.hpp"
    "include/Assisi/Editor/Overlay/IconPass.hpp"
    "include/Assisi/Editor/Overlay/LinePass.hpp"
    "include/Assisi/Editor/Overlay/OutlinePass.hpp"
    "include/Assisi/Editor/Overlay/OverlayRenderer.hpp"
    "include/Assisi/Editor/PrePlayState.hpp"
    "include/Assisi/Editor/SystemSearch.hpp"
    "include/Assisi/Editor/SourceAssets.hpp"
    "include/Assisi/Editor/TextureFile.hpp"
    "include/Assisi/Editor/ThumbnailCache.hpp"
    "include/Assisi/Editor/WireShapes.hpp"
  PRIVATE
    "src/ColliderPose.cpp"
    "src/EditHistory.cpp"
    "src/EditorApp.cpp"
    "src/SourceAssets.cpp"
    "src/TextureFile.cpp"
    "src/ThumbnailCache.cpp"
    "src/EditorAssetBrowser.cpp"
    "src/EditorBlueprintMode.cpp"
    "src/EditorCamera.cpp"
    "src/EditorCameraPaths.cpp"
    "src/EditorChiaraPanel.cpp"
    "src/EditorColliders.cpp"
    "src/EditorGizmo.cpp"
    "src/EditorInspector.cpp"
    "src/EditorLevels.cpp"
    "src/EditorLightGizmos.cpp"
    "src/EditorMaterialPanel.cpp"
    "src/EditorOptions.cpp"
    "src/EditorOptionsPanel.hpp"
    "src/EditorPlay.cpp"
    "src/EditorSystemsPanel.cpp"
    "src/EditorWorlds.cpp"
    "src/GizmoDrag.cpp"
    "src/InspectorFieldChrome.cpp"
    "src/InstanceGesture.cpp"
    # The overlay draw passes. They live here rather than in Render because a
    # game draws none of them: a selection outline, an entity icon and a
    # wireframe all mark something for whoever is editing, and Render holding
    # them meant every executable linked them.
    "src/Overlay/IconPass.cpp"
    "src/Overlay/LinePass.cpp"
    "src/Overlay/OutlinePass.cpp"
    "src/Overlay/OverlayRenderer.cpp"
    "src/PrePlayState.cpp"
    "src/ScenePick.cpp"
    "src/SystemSearch.cpp"
    "src/WireShapes.cpp"
    "src/ImGuiQueries.hpp"
)

target_include_directories(Assisi-Editor
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Editor
  PUBLIC
    # EditorApp derives from App::Application (its header needs App), and App
    # publicly brings Render, Physics, Window, ECS and Core. Runtime is also a
    # direct dependency of EditHistory, kept explicit.
    Assisi::App
    Assisi::Runtime
    # The UI toolkit and its platform/renderer backends. Linked here rather than
    # inherited from App, which deliberately links no UI at all: this is the
    # module that draws one, and it is the boundary a game does not cross.
    Assisi::Debug
    nlohmann_json::nlohmann_json
    # The source importers and image codecs: the editor reads the files an
    # author edits, which a game never links.
    Assisi::GeometryImport
    Assisi::ImageImport
    Assisi::FontImport
    # Screens: the editor compiles a screen file in process and reads the bytes
    # it just produced, so play-in-editor shows what a player would get.
    Assisi::MondrianImport
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
