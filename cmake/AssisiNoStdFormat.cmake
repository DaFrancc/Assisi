# Fails when engine code calls std::format, std::format_to or std::vformat
# instead of Core::Format. Each such call compiles the whole formatting engine
# into its file again, about three seconds of an optimised compile; Core::Format
# hands the work to Core::VFormat, compiled once.
#
# Run as a test: cmake -DSOURCE_DIR=<repository> -P AssisiNoStdFormat.cmake
#
# Sigil and Chiara do not link Core, so they keep the standard functions.
# Core/src/Format.cpp is where the one std::vformat lives.

file(GLOB_RECURSE _sources
     "${SOURCE_DIR}/modules/*.cpp" "${SOURCE_DIR}/modules/*.hpp"
     "${SOURCE_DIR}/apps/*.cpp" "${SOURCE_DIR}/apps/*.hpp")
list(FILTER _sources EXCLUDE REGEX "/modules/(Sigil|Chiara)/")
list(FILTER _sources EXCLUDE REGEX "/apps/sglc/")
list(FILTER _sources EXCLUDE REGEX "/modules/Core/src/Format\\.cpp$")

set(_found "")
foreach (_source IN LISTS _sources)
  file(STRINGS "${_source}" _lines REGEX "std::(format|format_to|vformat)\\(")
  foreach (_line IN LISTS _lines)
    file(RELATIVE_PATH _relative "${SOURCE_DIR}" "${_source}")
    string(STRIP "${_line}" _line)
    string(APPEND _found "  ${_relative}: ${_line}\n")
  endforeach()
endforeach()

if (_found)
  message(FATAL_ERROR "These call std::format; call Core::Format from <Assisi/Core/Format.hpp> instead, "
                      "which compiles the formatting engine once rather than in every file:\n${_found}")
endif()
