add_library(Assisi-Physics)
Assisi_apply_defaults(Assisi-Physics)

target_sources(Assisi-Physics
  PUBLIC
    "include/Assisi/Physics/PhysicsComponents.hpp"
    "include/Assisi/Physics/PhysicsWorld.hpp"
  PRIVATE
    "src/PhysicsWorld.cpp"
)

target_include_directories(Assisi-Physics
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Physics
  PUBLIC
    Assisi::Core
    Assisi::ECS
    Assisi::Runtime
    Jolt::Jolt
)

add_library(Assisi::Physics ALIAS Assisi-Physics)