# Copies the game into a fresh directory, with a content package beside it under
# the name a shipped build reads, or with none, and the Steam runtime library
# when the game was built with one.
#
#   cmake -DGAME=<executable> -DDIR=<directory> [-DPAK=<package>] [-DSTEAM_LIB=<library>] -P StageGame.cmake
#
# assets.pak is GameApp's kDefaultPakName. The files are named one by one, so
# nothing else beside the built game — steam_appid.txt above all, which Valve
# says never to ship — is ever staged.

file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}")
file(COPY "${GAME}" DESTINATION "${DIR}")
if (PAK)
  file(COPY_FILE "${PAK}" "${DIR}/assets.pak")
endif()
if (STEAM_LIB)
  file(COPY "${STEAM_LIB}" DESTINATION "${DIR}")
endif()
