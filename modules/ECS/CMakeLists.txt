add_library(Assisi-ECS)
Assisi_apply_defaults(Assisi-ECS)

target_sources(Assisi-ECS
  PUBLIC
    "include/Assisi/ECS/BlueprintMember.hpp"
    "include/Assisi/ECS/Entity.hpp"
    "include/Assisi/ECS/EntityRef.hpp"
    "include/Assisi/ECS/Hierarchy.hpp"
    "include/Assisi/ECS/Query.hpp"
    "include/Assisi/ECS/Registry.hpp"
    "include/Assisi/ECS/Scene.hpp"
    "include/Assisi/ECS/SparseSet.hpp"
    "include/Assisi/ECS/Transform.hpp"
    "include/Assisi/ECS/TransformPose.hpp"
  PRIVATE
    "src/EntityRef.cpp"
    "src/Hierarchy.cpp"
    "src/Registry.cpp"
    "src/TransformPose.cpp"
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

# Transform, Parent and BlueprintMember are reflected components; their
# registration is generated here rather than in Runtime so that renderer-free
# consumers get them without linking Runtime/Render — Physics for Transform and
# Parent, NetSync for all three.
assisi_reflect(
  TARGET Assisi-ECS
  HEADERS
    include/Assisi/ECS/BlueprintMember.hpp
    include/Assisi/ECS/Hierarchy.hpp
    include/Assisi/ECS/Transform.hpp
)

add_library(Assisi::ECS ALIAS Assisi-ECS)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
