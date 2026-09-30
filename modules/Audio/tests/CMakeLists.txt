add_executable(Assisi-Audio-Tests
  main.cpp
  TestAudioContext.cpp
  TestAudioDevice.cpp
  TestBusLayout.cpp
  TestClipDecode.cpp
  TestMixer.cpp
  TestSoundKind.cpp
)

# The sound kind and its cook step, taken directly rather than through
# assisi_link_reflections: a static library would drop their registrations.
target_sources(Assisi-Audio-Tests PRIVATE
  $<TARGET_OBJECTS:Assisi-Audio-AssetKind>
  $<TARGET_OBJECTS:Assisi-Audio-AssetCook>
)

Assisi_apply_test_defaults(Assisi-Audio-Tests)

target_include_directories(Assisi-Audio-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

# For AudioDeviceTestAccess.hpp only: the one way a test can lose a device.
target_include_directories(Assisi-Audio-Tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)

target_compile_definitions(Assisi-Audio-Tests PRIVATE
  ASSISI_AUDIO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(Assisi-Audio-Tests
  PRIVATE
    Assisi::Audio
)

add_test(NAME Audio COMMAND Assisi-Audio-Tests)

set_target_properties(Assisi-Audio-Tests PROPERTIES FOLDER "tests")
