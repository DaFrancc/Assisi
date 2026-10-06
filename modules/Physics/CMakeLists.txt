add_library(Assisi-Physics)
Assisi_apply_defaults(Assisi-Physics)

target_sources(Assisi-Physics
  PUBLIC
    "include/Assisi/Physics/ColliderRole.hpp"
    "include/Assisi/Physics/CollisionChannelNames.hpp"
    "include/Assisi/Physics/CollisionSource.hpp"
    "include/Assisi/Physics/PhysicsComponents.hpp"
    "include/Assisi/Physics/PhysicsWorld.hpp"
  PRIVATE
    "src/PhysicsInternal.hpp"
    "src/ColliderRole.cpp"
    "src/CollisionChannelNames.cpp"
    "src/JoltRuntime.cpp"
    "src/PhysicsWorld.cpp"
    "src/PhysicsCook.cpp"
    "src/PhysicsContacts.cpp"
    "src/PhysicsJoints.cpp"
    "src/PhysicsParts.cpp"
    "src/PhysicsQueries.cpp"
    "src/PhysicsReconcile.cpp"
    "src/PhysicsCharacters.cpp"
    "src/PhysicsWriteback.cpp"
)

target_include_directories(Assisi-Physics
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Physics
  PUBLIC
    Assisi::Core
    Assisi::ECS
    # Geometry: the collision a model carries, which a CollisionSource hands over.
    Assisi::Geometry
    Jolt::Jolt
)

assisi_reflect(
  TARGET Assisi-Physics
  HEADERS
    include/Assisi/Physics/PhysicsComponents.hpp
)

add_library(Assisi::Physics ALIAS Assisi-Physics)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()