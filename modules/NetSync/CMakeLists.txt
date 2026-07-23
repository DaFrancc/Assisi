add_library(Assisi-NetSync)
Assisi_apply_defaults(Assisi-NetSync)

target_sources(Assisi-NetSync
  PUBLIC
    "include/Assisi/NetSync/InputCommand.hpp"
    "include/Assisi/NetSync/NetClock.hpp"
  PRIVATE
    "src/InputCommand.cpp"
    "src/NetClock.cpp"
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
    Assisi::Net
)

add_library(Assisi::NetSync ALIAS Assisi-NetSync)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
