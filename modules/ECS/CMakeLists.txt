add_library(Assisi-ECS)
Assisi_apply_defaults(Assisi-ECS)

target_sources(Assisi-ECS
  PUBLIC
    "include/Assisi/ECS/Entity.hpp"
    "include/Assisi/ECS/Query.hpp"
    "include/Assisi/ECS/Registry.hpp"
    "include/Assisi/ECS/Scene.hpp"
    "include/Assisi/ECS/SceneRegistry.hpp"
    "include/Assisi/ECS/SparseSet.hpp"
    "include/Assisi/ECS/Transform.hpp"
  PRIVATE
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
)

# Transform is a reflected component; its registration is generated here rather
# than in Runtime so that renderer-free consumers (Physics) get it without
# linking Runtime/Render.
assisi_reflect(
  TARGET Assisi-ECS
  HEADERS
    include/Assisi/ECS/Transform.hpp
)

add_library(Assisi::ECS ALIAS Assisi-ECS)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
