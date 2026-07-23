add_library(Assisi-App)
Assisi_apply_defaults(Assisi-App)

target_sources(Assisi-App
  PUBLIC
    "include/Assisi/App/AppConfig.hpp"
    "include/Assisi/App/Application.hpp"
    "include/Assisi/App/LevelRuntime.hpp"
    "include/Assisi/App/OptionsConfig.hpp"
    "include/Assisi/App/SystemRegistry.hpp"
    "include/Assisi/App/World.hpp"
  PRIVATE
    "src/AppConfig.cpp"
    "src/Application.cpp"
    "src/LevelRuntime.cpp"
    "src/OptionsConfig.cpp"
    "src/SystemRegistry.cpp"
    "src/World.cpp"
)

target_include_directories(Assisi-App
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-App
  PUBLIC
    Assisi::Core
    Assisi::Window
    Assisi::Render
    Assisi::Debug
    Assisi::ECS
    Assisi::Runtime
    Assisi::Physics
    Assisi::Deps
  PRIVATE
    nlohmann_json::nlohmann_json
)

add_library(Assisi::App ALIAS Assisi-App)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()