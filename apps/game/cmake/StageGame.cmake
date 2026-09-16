# Copies the game into a fresh directory, with a content package beside it under
# the name a shipped build reads, or with none.
#
#   cmake -DGAME=<executable> -DDIR=<directory> [-DPAK=<package>] -P StageGame.cmake
#
# assets.pak is GameApp's kDefaultPakName.

file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}")
file(COPY "${GAME}" DESTINATION "${DIR}")
if (PAK)
  file(COPY_FILE "${PAK}" "${DIR}/assets.pak")
endif()
