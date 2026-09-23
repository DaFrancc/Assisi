add_library(Assisi-App)
Assisi_apply_defaults(Assisi-App)

target_sources(Assisi-App
  PUBLIC
    "include/Assisi/App/AppConfig.hpp"
    "include/Assisi/App/Application.hpp"
    "include/Assisi/App/BlueprintVerbs.hpp"
    "include/Assisi/App/CameraBenchmark.hpp"
    "include/Assisi/App/ChildProcess.hpp"
    "include/Assisi/App/ContentSet.hpp"
    "include/Assisi/App/CookedAssets.hpp"
    "include/Assisi/App/GameApp.hpp"
    "include/Assisi/App/InputSetup.hpp"
    "include/Assisi/App/LevelRuntime.hpp"
    "include/Assisi/App/OptionsConfig.hpp"
    "include/Assisi/App/SceneCamera.hpp"
    "include/Assisi/App/PhysicsSystems.hpp"
    "include/Assisi/App/MotionSystems.hpp"
    "include/Assisi/App/SystemCatalog.hpp"
    "include/Assisi/App/SystemRegistry.hpp"
    "include/Assisi/App/UiInputBridge.hpp"
    "include/Assisi/App/World.hpp"
  PRIVATE
    "src/AppConfig.cpp"
    "src/Application.cpp"
    "src/BlueprintVerbs.cpp"
    $<$<BOOL:${ASSISI_ENABLE_NETWORKING}>:src/BlueprintReplication.cpp>
    "src/CameraBenchmark.cpp"
    "src/ChildProcess.cpp"
    "src/ChiaraCapture.cpp"
    "src/ContentSet.cpp"
    "src/CookedAssets.cpp"
    "src/CrashReport.cpp"
    "src/GameApp.cpp"
    "src/InputSetup.cpp"
    "src/LevelRuntime.cpp"
    "src/OptionsConfig.cpp"
    "src/SceneCamera.cpp"
    "src/StartupScene.cpp"
    "src/PerfCapture.cpp"
    "src/MotionSystems.cpp"
    "src/PhysicsSystems.cpp"
    "src/SystemCatalog.cpp"
    "src/SystemRegistry.cpp"
    "src/UiInputBridge.cpp"
    "src/World.cpp"
)

target_include_directories(Assisi-App
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Assisi::Debug is deliberately absent, and so is any debug UI toolkit.
# Application owns the game UI, Mondrian, and draws it before the capture point;
# OnRenderUi hands the last slot in the frame to whoever overrode it, and that
# subclass links what it draws with. That is what lets the Game executable ship
# without the editor's UI.
target_link_libraries(Assisi-App
  PUBLIC
    Assisi::Core
    Assisi::Window
    Assisi::Render
    Assisi::ECS
    Assisi::Runtime
    Assisi::Physics
    Assisi::MondrianEngine
    Assisi::Deps
  PRIVATE
    nlohmann_json::nlohmann_json
)

# Only when networking is on, and only for BlueprintReplication.cpp — the
# adapters that let NetSync replicate blueprints without knowing what one is.
# Acyclic: NetSync links Core/ECS/Math/Net/Physics and neither App nor Runtime,
# which is exactly why the provider/expander seams exist.
if (ASSISI_ENABLE_NETWORKING)
  target_link_libraries(Assisi-App PUBLIC Assisi::NetSync)
endif()

# The engine's own systems and events. A declaration in a scanned header *is* the
# registration — the generated OBJECT library is pulled fully into every final
# link, so a level naming "Bounce" finds it without anyone calling a registrar.
#
# SystemRegistry.hpp is here for its AEVENT alone: QuitRequested is what a Quit
# button in a screen file names, and an event declared in a header nothing scans
# is an event no file can push.
assisi_reflect(
  TARGET Assisi-App
  HEADERS
    include/Assisi/App/AppConfig.hpp
    include/Assisi/App/PhysicsSystems.hpp
    include/Assisi/App/MotionSystems.hpp
    include/Assisi/App/SystemRegistry.hpp
)

add_library(Assisi::App ALIAS Assisi-App)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()