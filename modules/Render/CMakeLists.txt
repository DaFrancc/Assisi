add_library(Assisi-Render)
Assisi_apply_defaults(Assisi-Render)

target_sources(Assisi-Render
  PRIVATE
    "src/shader.cpp"
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
