add_library(Assisi-Render)
Assisi_apply_defaults(Assisi-Render)

target_sources(Assisi-Render
  PUBLIC
    "include/Assisi/Render/AssetCache.hpp"
    "include/Assisi/Render/Buffer.hpp"
    "include/Assisi/Render/DrawItem.hpp"
    "include/Assisi/Render/Frustum.hpp"
    "include/Assisi/Render/GpuTelemetry.hpp"
    "include/Assisi/Render/Material.hpp"
    "include/Assisi/Render/MeshBuffer.hpp"
    "include/Assisi/Render/MeshPass.hpp"
    "include/Assisi/Render/PostProcess.hpp"
    "include/Assisi/Render/RenderFrame.hpp"
    "include/Assisi/Render/RenderSystem.hpp"
    "include/Assisi/Render/ShaderModule.hpp"
    "include/Assisi/Render/Texture.hpp"
    "include/Assisi/Render/ComputeShader.hpp"
    "include/Assisi/Render/ClusterGrid.hpp"
    "include/Assisi/Render/Vulkan/VulkanContext.hpp"
  PRIVATE
    "src/AssetCache.cpp"
    "src/ShaderModule.cpp"
    "src/ClusterGrid.cpp"
    "src/ComputeShader.cpp"
    "src/Buffer.cpp"
    "src/Material.cpp"
    "src/Frustum.cpp"
    "src/GpuTelemetry.cpp"
    "src/MeshPass.cpp"
    "src/OutlinePass.cpp"
    "src/PostProcess.cpp"
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
    # Geometry owns MeshData (exposed in MeshBuffer's API) and the mesh importer.
    Assisi::Geometry
    Assisi::Window
    Assisi::Deps
    # nvrhi_vk (Vulkan backend) references symbols defined in nvrhi (common),
    # so it must precede nvrhi on the link line for single-pass static linkers (ld).
    nvrhi_vk
    nvrhi
    Vulkan::Headers
  PRIVATE
    # GpuTelemetry dlopen()s NVML at runtime; dl is needed on Linux (no-op alias
    # on platforms that fold it into libc).
    ${CMAKE_DL_LIBS}
)

add_library(Assisi::Render ALIAS Assisi-Render)
