add_executable(Assisi-Audio-Tests
  main.cpp
  TestAudioContext.cpp
  TestAudioDevice.cpp
  TestClipDecode.cpp
)

Assisi_apply_test_defaults(Assisi-Audio-Tests)

target_include_directories(Assisi-Audio-Tests SYSTEM PRIVATE
  "${doctest_SOURCE_DIR}"
)

# For AudioDeviceTestAccess.hpp only: the one way a test can lose a device.
target_include_directories(Assisi-Audio-Tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)

target_link_libraries(Assisi-Audio-Tests
  PRIVATE
    Assisi::Audio
)

add_test(NAME Audio COMMAND Assisi-Audio-Tests)

set_target_properties(Assisi-Audio-Tests PROPERTIES FOLDER "tests")
