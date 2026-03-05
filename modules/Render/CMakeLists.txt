add_library(Assisi-Render)
Assisi_apply_defaults(Assisi-Render)

target_sources(Assisi-Render
  PUBLIC
    "include/Assisi/Render/Backend/GraphicsBackend.hpp"
    "include/Assisi/Render/OpenGL/DefaultTextures.hpp"
    "include/Assisi/Render/OpenGL/MeshBuffer.hpp"
    "include/Assisi/Render/Buffer.hpp"
    "include/Assisi/Render/DefaultMeshes.hpp"
    "include/Assisi/Render/DefaultResources.hpp"
    "include/Assisi/Render/MeshData.hpp"
    "include/Assisi/Render/RenderSystem.hpp"
    "include/Assisi/Render/Shader.hpp"
  PRIVATE
    "src/Shader.cpp"
    "src/Buffer.cpp"
    "src/DefaultResources.cpp"
    "src/RenderSystemOpenGL.cpp"
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
)

add_library(Assisi::Render ALIAS Assisi-Render)
