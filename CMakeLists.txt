cmake_minimum_required(VERSION 3.22)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
    "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,\
$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,\
$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project(Assisi LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(AssisiOptions)
include(AssisiModule)

add_subdirectory(modules/core)
# add_subdirectory(modules/ecs)
# add_subdirectory(modules/runtime)

add_subdirectory(apps/sandbox)
