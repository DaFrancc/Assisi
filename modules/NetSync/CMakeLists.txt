add_library(Assisi-NetSync)
Assisi_apply_defaults(Assisi-NetSync)

target_sources(Assisi-NetSync
  PUBLIC
    "include/Assisi/NetSync/InputCommand.hpp"
    "include/Assisi/NetSync/NetClock.hpp"
    "include/Assisi/NetSync/NetComponents.hpp"
    "include/Assisi/NetSync/NetProtocol.hpp"
    "include/Assisi/NetSync/NetSession.hpp"
    "include/Assisi/NetSync/Replication.hpp"
  PRIVATE
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

# Core + ECS + Net, and deliberately not Runtime: Runtime links Render, which
# would poison the headless server's link. NetSync mirrors how Physics sits —
# simulation-side only.
target_link_libraries(Assisi-NetSync
  PUBLIC
    Assisi::Core
    Assisi::ECS
    Assisi::Math
    Assisi::Net
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
