add_library(Assisi-Render)
Assisi_apply_defaults(Assisi-Render)

target_sources(Assisi-Render
  PUBLIC
    "include/Assisi/Render/OpenGL/MeshBuffer.hpp"
    "include/Assisi/Render/Buffer.hpp"
    "include/Assisi/Render/DefaultMeshes.hpp"
    "include/Assisi/Render/DefaultResources.hpp"
    "include/Assisi/Render/MeshBuffer.hpp"
    "include/Assisi/Render/MeshData.hpp"
    "include/Assisi/Render/MeshPass.hpp"
    "include/Assisi/Render/RenderSystem.hpp"
    "include/Assisi/Render/ShaderModule.hpp"
    "include/Assisi/Render/Texture.hpp"
    "include/Assisi/Render/ComputeShader.hpp"
    "include/Assisi/Render/ClusterGrid.hpp"
    "include/Assisi/Render/Vulkan/VulkanContext.hpp"
  PRIVATE
    "src/ShaderModule.cpp"
    "src/ClusterGrid.cpp"
    "src/ComputeShader.cpp"
    "src/Buffer.cpp"
    "src/DefaultResources.cpp"
    "src/MeshPass.cpp"
    "src/Texture.cpp"
    "src/VulkanContext.cpp"
)

target_include_directories(Assisi-Render
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Render
  PUBLIC
    Assisi::Core
    Assisi::Math
    Assisi::Window
    Assisi::Deps
    nvrhi
    nvrhi_vk
    Vulkan::Headers
)

add_library(Assisi::Render ALIAS Assisi-Render)
