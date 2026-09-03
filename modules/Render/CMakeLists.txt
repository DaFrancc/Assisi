add_library(Assisi-Render)
Assisi_apply_defaults(Assisi-Render)

target_sources(Assisi-Render
  PUBLIC
    "include/Assisi/Render/AssetCache.hpp"
    "include/Assisi/Render/Buffer.hpp"
    "include/Assisi/Render/DrawItem.hpp"
    "include/Assisi/Render/Frustum.hpp"
    "include/Assisi/Render/GpuLayout.hpp"
    "include/Assisi/Render/GpuTelemetry.hpp"
    "include/Assisi/Render/IndirectLighting.hpp"
    "include/Assisi/Render/LocalShadowPass.hpp"
    "include/Assisi/Render/Material.hpp"
    "include/Assisi/Render/LinePass.hpp"
    "include/Assisi/Render/MeshBuffer.hpp"
    "include/Assisi/Render/MeshCuller.hpp"
    "include/Assisi/Render/MeshPass.hpp"
    "include/Assisi/Render/PostProcess.hpp"
    "include/Assisi/Render/RenderFrame.hpp"
    "include/Assisi/Render/RenderSystem.hpp"
    "include/Assisi/Render/ShaderModule.hpp"
    "include/Assisi/Render/ShadowAtlas.hpp"
    "include/Assisi/Render/ShadowCascades.hpp"
    "include/Assisi/Render/ShadowDepthRenderer.hpp"
    "include/Assisi/Render/ShadowImportance.hpp"
    "include/Assisi/Render/ShadowPass.hpp"
    "include/Assisi/Render/ShadowSettings.hpp"
    "include/Assisi/Render/ShadowView.hpp"
    "include/Assisi/Render/Sky.hpp"
    "include/Assisi/Render/SkyPass.hpp"
    "include/Assisi/Render/Texture.hpp"
    "include/Assisi/Render/Tonemap.hpp"
    "include/Assisi/Render/ComputeShader.hpp"
    "include/Assisi/Render/ClusterGrid.hpp"
    "include/Assisi/Render/Vulkan/VulkanContext.hpp"
  PRIVATE
    "src/AssetCache.cpp"
    "src/ShaderModule.cpp"
    "src/ClusterGrid.cpp"
    "src/ComputeShader.cpp"
    "src/Buffer.cpp"
    "src/LocalShadowPass.cpp"
    "src/Material.cpp"
    "src/MeshCuller.cpp"
    "src/Frustum.cpp"
    "src/GpuMarker.cpp"
    "src/GpuTelemetry.cpp"
    "src/MeshPass.cpp"
    "src/OutlinePass.cpp"
    "src/IconPass.cpp"
    "src/LinePass.cpp"
    "src/PostProcess.cpp"
    "src/ShadowAtlas.cpp"
    "src/ShadowCascades.cpp"
    "src/ShadowDepthRenderer.cpp"
    "src/ShadowImportance.cpp"
    "src/ShadowPass.cpp"
    "src/ShadowView.cpp"
    "src/SkyPass.cpp"
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
    # WebP decode (Texture::DecodeAnimatedWebp). webpdemux pulls in the base
    # `webp` decoder it depends on, so listing it alone is enough.
    webpdemux
)

# PUBLIC for the same reason Chiara's gate is: GpuMarker.hpp is header-only, so
# every translation unit that includes it must agree with this library about
# whether the marker type exists.
if (ASSISI_ENABLE_GPU_MARKERS)
  target_compile_definitions(Assisi-Render PUBLIC ASSISI_ENABLE_GPU_MARKERS=1)
endif()

add_library(Assisi::Render ALIAS Assisi-Render)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
