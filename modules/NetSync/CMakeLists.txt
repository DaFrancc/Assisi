add_library(Assisi-NetSync)
Assisi_apply_defaults(Assisi-NetSync)

target_sources(Assisi-NetSync
  PUBLIC
    "include/Assisi/NetSync/BodyState.hpp"
    "include/Assisi/NetSync/DistanceRelevancy.hpp"
    "include/Assisi/NetSync/InputCommand.hpp"
    "include/Assisi/NetSync/NetClock.hpp"
    "include/Assisi/NetSync/NetComponents.hpp"
    "include/Assisi/NetSync/NetProtocol.hpp"
    "include/Assisi/NetSync/NetSession.hpp"
    "include/Assisi/NetSync/Replication.hpp"
  PRIVATE
    "src/BodyState.cpp"
    "src/DistanceRelevancy.cpp"
    "src/InputCommand.cpp"
    "src/NetClock.cpp"
    "src/NetProtocol.cpp"
    "src/NetSession.cpp"
    "src/Replication.cpp"
)

target_include_directories(Assisi-NetSync
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Core + ECS + Net + Physics, and deliberately not Runtime: Runtime links
# Render, which would poison the headless server's link. Physics is safe for the
# same reason it was the module NetSync was told to imitate — Core + ECS + Jolt,
# render-free — and under local simulation both halves of the protocol read and
# write bodies directly, because the render-side Transform is not the physics
# truth (docs/replication-plan-v4.md §3.3).
target_link_libraries(Assisi-NetSync
  PUBLIC
    Assisi::Core
    Assisi::ECS
    Assisi::Math
    Assisi::Net
    Assisi::Physics
)

assisi_reflect(
  TARGET Assisi-NetSync
  HEADERS
    include/Assisi/NetSync/NetComponents.hpp
)

add_library(Assisi::NetSync ALIAS Assisi-NetSync)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
