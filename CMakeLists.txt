cmake_minimum_required(VERSION 3.22)

if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
    "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,\
$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,\
$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project(Assisi LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(AssisiOptions)
include(AssisiModule)

set(ASSISI_EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external")

if (NOT EXISTS "${ASSISI_EXTERNAL_DIR}/glfw/CMakeLists.txt")
  message(FATAL_ERROR "Missing external/glfw. Run: git submodule update --init --recursive")
endif()

if (NOT EXISTS "${ASSISI_EXTERNAL_DIR}/glm/glm.hpp")
  message(FATAL_ERROR "Missing external/glm/glm/glm.hpp. Run: git submodule update --init --recursive")
endif()

if (NOT EXISTS "${ASSISI_EXTERNAL_DIR}/stb/stb_image.h")
  message(FATAL_ERROR "Missing external/stb/stb_image.h. Run: git submodule update --init --recursive")
endif()

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

add_subdirectory("${ASSISI_EXTERNAL_DIR}/glfw" "${CMAKE_BINARY_DIR}/glfw-build")

add_library(Assisi-stb INTERFACE)
target_include_directories(Assisi-stb
  INTERFACE
    "${ASSISI_EXTERNAL_DIR}/stb"
)

add_library(Assisi-glm INTERFACE)
target_include_directories(Assisi-glm
  INTERFACE
    "${ASSISI_EXTERNAL_DIR}/glm"
)

add_library(Assisi-glad STATIC
  "${ASSISI_EXTERNAL_DIR}/glad/src/glad.c"
)

target_include_directories(Assisi-glad
  PUBLIC
    "${ASSISI_EXTERNAL_DIR}/glad/include"
)

add_library(Assisi::glad ALIAS Assisi-glad)

add_library(Assisi-Deps INTERFACE)
target_link_libraries(Assisi-Deps
  INTERFACE
    glfw
    Assisi-stb
    Assisi-glm
    Assisi-glad
)

add_library(Assisi::Deps ALIAS Assisi-Deps)

add_subdirectory(modules/Core)
add_subdirectory(modules/Render)
add_subdirectory(modules/Scene)
add_subdirectory(modules/Window)
add_subdirectory(modules/Math)

add_subdirectory(apps/Sandbox)
