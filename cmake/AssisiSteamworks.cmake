# Finds the Steamworks SDK the developer installed with `./assisi steam-sdk`.
#
# The SDK is never in the repository and never downloaded by the build: Valve's
# agreement lets a developer copy it locally and ship its runtime libraries in a
# game, nothing more. So it is looked for in one gitignored folder at the root,
# where the install command puts it, or wherever ASSISI_STEAMWORKS_SDK points.
# Without it the engine builds as always and Steam reports itself unavailable.
#
# The search is a CONFIGURE_DEPENDS glob, so installing or removing the SDK makes
# the next build configure again by itself. The folder is inside the repository
# for the same reason the Steam Runtime build needs nothing extra: the container
# mounts the repository, so it sees the SDK too.
#
# Sets ASSISI_STEAMWORKS_ENABLED, ASSISI_STEAMWORKS_INCLUDE (the folder holding
# steam/steam_api.h) and the imported target Steamworks::steam_api.

set(ASSISI_STEAMWORKS_SDK "" CACHE PATH
    "A Steamworks SDK folder holding public/steam/ and redistributable_bin/; empty looks in steamworks/")

if (ASSISI_STEAMWORKS_SDK)
  set(_steamworks_root "${ASSISI_STEAMWORKS_SDK}")
else()
  set(_steamworks_root "${CMAKE_SOURCE_DIR}/steamworks")
endif()

file(GLOB _steamworks_header CONFIGURE_DEPENDS "${_steamworks_root}/public/steam/steam_api.h")

set(ASSISI_STEAMWORKS_ENABLED OFF)
if (_steamworks_header)
  if (WIN32)
    # Wired from the SDK's layout; Windows has not been built yet.
    set(_steamworks_library "${_steamworks_root}/redistributable_bin/win64/steam_api64.dll")
    set(_steamworks_import "${_steamworks_root}/redistributable_bin/win64/steam_api64.lib")
  else()
    set(_steamworks_library "${_steamworks_root}/redistributable_bin/linux64/libsteam_api.so")
  endif()
  if (NOT EXISTS "${_steamworks_library}")
    message(FATAL_ERROR "The Steamworks SDK in ${_steamworks_root} has headers but no ${_steamworks_library}. "
                        "Install it again with ./assisi steam-sdk <your download>.")
  endif()

  add_library(Steamworks::steam_api SHARED IMPORTED GLOBAL)
  set_target_properties(Steamworks::steam_api PROPERTIES IMPORTED_LOCATION "${_steamworks_library}")
  if (WIN32)
    set_target_properties(Steamworks::steam_api PROPERTIES IMPORTED_IMPLIB "${_steamworks_import}")
  endif()

  set(ASSISI_STEAMWORKS_ENABLED ON)
  set(ASSISI_STEAMWORKS_INCLUDE "${_steamworks_root}/public")
  set(_steamworks_version "")
  if (EXISTS "${_steamworks_root}/VERSION")
    file(STRINGS "${_steamworks_root}/VERSION" _steamworks_version LIMIT_COUNT 1)
  endif()
  message(STATUS "Steamworks SDK ${_steamworks_version}: ${_steamworks_root}")
elseif (ASSISI_STEAMWORKS_SDK)
  message(FATAL_ERROR "ASSISI_STEAMWORKS_SDK is ${ASSISI_STEAMWORKS_SDK}, which has no public/steam/steam_api.h.")
else()
  message(STATUS "Steamworks SDK: not installed, so Steam reports itself unavailable "
                 "(./assisi steam-sdk <your download> installs it)")
endif()
