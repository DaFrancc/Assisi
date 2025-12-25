# Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc")

cmake_minimum_required(VERSION 3.22)

# ============================================================
# MSVC debug info policy
# ============================================================

if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
    "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,\
$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,\
$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

# ============================================================
# Project
# ============================================================

project(Assisi LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(AssisiOptions)
include(AssisiModule)

# ============================================================
# Build knobs (driven by Makefile)
# ============================================================

option(ASSISI_DEBUG_WARNINGS "Enable strong debug warnings" OFF)
option(ASSISI_RELEASE_OPTIMIZED "Optimize aggressively for speed" OFF)
option(ASSISI_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ASSISI_ASAN_HARDCORE "Enable max sanitizer + warning set (Clang)" OFF)

# ============================================================
# Per-target flags (apply ONLY to Assisi targets)
# ============================================================

add_library(Assisi-Warnings INTERFACE)
add_library(Assisi-Sanitize INTERFACE)

# Debug warnings ("-Wall" equivalent)
if (ASSISI_DEBUG_WARNINGS)
  if (MSVC)
    target_compile_options(Assisi-Warnings INTERFACE
      /W4
      /permissive-
      /Zc:__cplusplus
      /Zc:preprocessor
      /EHsc

      # High-signal warnings (do NOT use /Wall)
      /w14640   # implicit fallthrough
      /w14242   # possible data loss
      /w14254   # operator precedence
      /w14263   # override
      /w14265   # virtual dtor
      /w14287   # signed/unsigned mix
      /w14296   # always true/false
    )
  else()
    target_compile_options(Assisi-Warnings INTERFACE
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wconversion
      -Wsign-conversion
      -Wnon-virtual-dtor
      -Woverloaded-virtual
      -Wnull-dereference
      -Wdouble-promotion
    )
  endif()
endif()

# ASan (Windows: clang-cl/MSVC-mode safe)
# IMPORTANT: don't pass /fsanitize=address to lld-link directly on your setup.
if (ASSISI_ENABLE_ASAN AND MSVC)
  target_compile_options(Assisi-Sanitize INTERFACE /fsanitize=address /Zi /Od)
endif()

# Extra warnings for Clang (safe on clang-cl too)
if (ASSISI_ASAN_HARDCORE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(Assisi-Sanitize INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow
    -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
  )
endif()

# Release optimization (speed)
if (ASSISI_RELEASE_OPTIMIZED AND MSVC)
  target_compile_options(Assisi-Warnings INTERFACE
    /O2
    /Ob3
    /Oi
    /Ot
    /GL
  )
  target_link_options(Assisi-Warnings INTERFACE /LTCG)
endif()

# ============================================================
# External dependencies
# ============================================================

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
  SYSTEM INTERFACE
    "${ASSISI_EXTERNAL_DIR}/stb"
)

add_library(Assisi-glm INTERFACE)
target_include_directories(Assisi-glm
  SYSTEM INTERFACE
    "${ASSISI_EXTERNAL_DIR}/glm"
)

add_library(Assisi-glad STATIC
  "${ASSISI_EXTERNAL_DIR}/glad/src/glad.c"
)

target_include_directories(Assisi-glad
  SYSTEM PUBLIC
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

# ============================================================
# Project modules / apps
# ============================================================

add_subdirectory(modules/Core)
add_subdirectory(modules/Render)
add_subdirectory(modules/ECS)
add_subdirectory(modules/Game)
add_subdirectory(modules/Window)
add_subdirectory(modules/Math)

add_subdirectory(apps/Sandbox)
