add_library(Assisi-Net)
Assisi_apply_defaults(Assisi-Net)

target_sources(Assisi-Net
  PUBLIC
    "include/Assisi/Net/NetTransport.hpp"
  PRIVATE
    "src/NetTransport.cpp"
)

target_include_directories(Assisi-Net
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# GameNetworkingSockets is PRIVATE, unlike Jolt in Physics: NetTransport pimpls
# every GNS type away, so nothing downstream needs its headers — and keeping the
# link private is what makes the "swap the transport without touching callers"
# claim in the design notes true rather than aspirational.
target_link_libraries(Assisi-Net
  PUBLIC
    Assisi::Core
  PRIVATE
    GameNetworkingSockets::static
)

add_library(Assisi::Net ALIAS Assisi-Net)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
