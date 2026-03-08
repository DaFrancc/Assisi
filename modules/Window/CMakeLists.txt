add_library(Assisi-Window)
Assisi_apply_defaults(Assisi-Window)

target_sources(Assisi-Window
  PUBLIC
    "include/Assisi/Window/ActionMap.hpp"
    "include/Assisi/Window/GlfwLibrary.hpp"
    "include/Assisi/Window/InputContext.hpp"
    "include/Assisi/Window/Key.hpp"
    "include/Assisi/Window/Window.hpp"
    "include/Assisi/Window/WindowContext.hpp"
  PRIVATE
    "src/ActionMap.cpp"
    "src/GlfwLibrary.cpp"
    "src/InputContext.cpp"
    "src/Window.cpp"
    "src/WindowContext.cpp"
)

target_include_directories(Assisi-Window
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Window
  PUBLIC
    Assisi::Core
    Assisi::Deps
  PRIVATE
    nlohmann_json::nlohmann_json
)

add_library(Assisi::Window ALIAS Assisi-Window)