add_library(Assisi-ECS)
Assisi_apply_defaults(Assisi-ECS)

target_sources(Assisi-ECS
  PUBLIC
    "include/Assisi/ECS/ECS.hpp"
    "include/Assisi/ECS/Entity.hpp"
    "include/Assisi/ECS/Registry.hpp"
    "include/Assisi/ECS/Scene.hpp"
    "include/Assisi/ECS/SceneRegistry.hpp"
    "include/Assisi/ECS/SparseSet.hpp"
  PRIVATE
    "src/ECS.cpp"
    "src/Registry.cpp"
    "src/SceneRegistry.cpp"
)

target_include_directories(Assisi-ECS
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-ECS
  PUBLIC
    Assisi::Core
    Assisi::Math
    Assisi::Render
)

add_library(Assisi::ECS ALIAS Assisi-ECS)
