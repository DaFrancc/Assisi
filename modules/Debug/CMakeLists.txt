add_library(Assisi-Debug)
Assisi_apply_defaults(Assisi-Debug)

target_sources(Assisi-Debug
  PUBLIC
    "include/Assisi/Debug/DebugUI.hpp"
  PRIVATE
    "src/DebugUI.cpp"
    # Dear ImGui platform/renderer backends (compiled as part of this module)
    "${ASSISI_IMGUI_ROOT}/backends/imgui_impl_glfw.cpp"
    "${ASSISI_IMGUI_ROOT}/backends/imgui_impl_vulkan.cpp"
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
    implot::implot
    Assisi::Window
    Assisi::Render
  PRIVATE
    Assisi::Deps
)

# No Vulkan SDK in this dev environment — imgui_impl_vulkan.cpp must load its
# own Vulkan function pointers rather than link a static import lib. Applies to
# the whole target since both imgui_impl_vulkan.cpp and DebugUI.cpp need it.
target_compile_definitions(Assisi-Debug PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES)

add_library(Assisi::Debug ALIAS Assisi-Debug)