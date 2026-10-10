add_library(Assisi-Steam)
Assisi_apply_defaults(Assisi-Steam)

target_sources(Assisi-Steam
  PUBLIC
    "include/Assisi/Steam/Client.hpp"
    "include/Assisi/Steam/Steam.hpp"
  PRIVATE
    "src/Unavailable.cpp"
)

# One of two bodies for Connect: the real one with the developer's local SDK,
# or one that always reports the engine was built without it. The SDK's include
# path is PRIVATE and SYSTEM: only Steamworks.cpp sees Valve's headers, so they
# cannot meet the GameNetworkingSockets headers of the same names in any other
# file, and their warnings are not this tree's to fix.
if (ASSISI_STEAMWORKS_ENABLED)
  target_sources(Assisi-Steam PRIVATE "src/Steamworks.cpp")
  target_include_directories(Assisi-Steam SYSTEM PRIVATE "${ASSISI_STEAMWORKS_INCLUDE}")
  target_link_libraries(Assisi-Steam PRIVATE Steamworks::steam_api)
else()
  target_sources(Assisi-Steam PRIVATE "src/ConnectStub.cpp")
endif()

target_include_directories(Assisi-Steam
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Steam
  PUBLIC
    # Core: the Logger the backend reports through.
    Assisi::Core
)

add_library(Assisi::Steam ALIAS Assisi-Steam)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
