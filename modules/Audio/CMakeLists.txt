add_library(Assisi-Audio)
Assisi_apply_defaults(Assisi-Audio)

target_sources(Assisi-Audio
  PUBLIC
    "include/Assisi/Audio/AudioContext.hpp"
    "include/Assisi/Audio/AudioDevice.hpp"
    "include/Assisi/Audio/AudioDeviceInfo.hpp"
    "include/Assisi/Audio/AudioError.hpp"
    "include/Assisi/Audio/AudioFormat.hpp"
    "include/Assisi/Audio/AudioRenderer.hpp"
    "include/Assisi/Audio/Clip.hpp"
  PRIVATE
    "src/AudioContext.cpp"
    "src/AudioContextImpl.hpp"
    "src/AudioDevice.cpp"
    "src/AudioDeviceTestAccess.hpp"
    "src/AudioError.cpp"
    "src/Clip.cpp"
)

target_include_directories(Assisi-Audio
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Audio
  PUBLIC
    # Core: the Logger a device reports a move to another device through.
    Assisi::Core
  PRIVATE
    # Hidden behind pimpls: a consumer plays samples and must not inherit the
    # backend's header to do it.
    miniaudio::miniaudio
)

add_library(Assisi::Audio ALIAS Assisi-Audio)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
