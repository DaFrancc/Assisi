add_library(Assisi-Debug)
Assisi_apply_defaults(Assisi-Debug)

target_sources(Assisi-Debug
  PUBLIC
    "include/Assisi/Debug/DebugUI.hpp"
  PRIVATE
    "src/DebugUI.cpp"
    # Dear ImGui platform/renderer backends (compiled as part of this module)
    "${ASSISI_IMGUI_ROOT}/backends/imgui_impl_glfw.cpp"
    "${ASSISI_IMGUI_ROOT}/backends/imgui_impl_opengl3.cpp"
)

target_include_directories(Assisi-Debug
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE
    # Backend headers live alongside their source files
    "${ASSISI_IMGUI_ROOT}/backends"
)

target_link_libraries(Assisi-Debug
  PUBLIC
    imgui::imgui
    Assisi::Window
  PRIVATE
    Assisi::Deps
)

add_library(Assisi::Debug ALIAS Assisi-Debug)