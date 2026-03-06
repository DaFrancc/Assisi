add_library(Assisi-Runtime)
Assisi_apply_defaults(Assisi-Runtime)

target_sources(Assisi-Runtime
  PUBLIC
    "include/Assisi/Runtime/Camera.hpp"
    "include/Assisi/Runtime/Components.hpp"
    "include/Assisi/Runtime/LightComponents.hpp"
    "include/Assisi/Runtime/LightingSystem.hpp"
    "include/Assisi/Runtime/Renderer.hpp"
    "include/Assisi/Runtime/Transform.hpp"
  PRIVATE
    "src/Camera.cpp"
    "src/LightingSystem.cpp"
    "src/Renderer.cpp"
)

target_include_directories(Assisi-Runtime
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Runtime
  PUBLIC
    Assisi::Core
    Assisi::ECS
    Assisi::Math
    Assisi::Render
)

add_library(Assisi::Runtime ALIAS Assisi-Runtime)
