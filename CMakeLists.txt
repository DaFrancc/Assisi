cmake_minimum_required(VERSION 3.23)

project(Assisi
  VERSION 0.1.0
  LANGUAGES C CXX
)

# ------------------------------------------------------------
# Global language policy
# ------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Tooling (reflection/codegen can consume compile_commands.json later)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ------------------------------------------------------------
# Project-wide knobs (driven by presets/CI, not hand-edited cache)
# ------------------------------------------------------------
option(ASSISI_WARNINGS_AS_ERRORS "Treat warnings as errors (recommended for CI)" OFF)

# Sanitizers: primarily valuable on Linux; MSVC support is limited.
option(ASSISI_ENABLE_SANITIZERS "Enable sanitizers for this build" OFF)
set(ASSISI_SANITIZER_SET "address;undefined" CACHE STRING
    "Semicolon-separated list: address;undefined;thread (support varies)")

# Release performance knobs
option(ASSISI_ENABLE_FAST_MATH "Enable fast-math in Release" ON)

# ------------------------------------------------------------
# Central interface targets (clean and explicit)
# ------------------------------------------------------------
add_library(Assisi-Options INTERFACE)
add_library(Assisi-Warnings INTERFACE)
add_library(Assisi-Sanitize INTERFACE)
add_library(Assisi-Perf INTERFACE)

add_library(Assisi::Options  ALIAS Assisi-Options)
add_library(Assisi::Warnings ALIAS Assisi-Warnings)
add_library(Assisi::Sanitize ALIAS Assisi-Sanitize)
add_library(Assisi::Perf     ALIAS Assisi-Perf)

function(Assisi_apply_defaults target_name)
  if (NOT TARGET "${target_name}")
    message(FATAL_ERROR "Assisi_apply_defaults: target '${target_name}' does not exist")
  endif()

  target_link_libraries("${target_name}"
    PUBLIC
      Assisi::Options
      Assisi::Warnings
      Assisi::Perf
      Assisi::Sanitize
  )
endfunction()


# ---- Baseline compile behavior (applies to targets only when Options are linked)
if (MSVC)
  target_compile_options(Assisi-Options INTERFACE
    /permissive-
    /Zc:__cplusplus
    /Zc:preprocessor
    /EHsc
  )
  target_compile_definitions(Assisi-Options INTERFACE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
  )
else()
  target_compile_options(Assisi-Options INTERFACE
    -fno-omit-frame-pointer
  )
endif()

# ---- Warnings (always ON; warnings-as-errors is a separate switch)
if (MSVC)
  target_compile_options(Assisi-Warnings INTERFACE
    /W4
    /w14640   # implicit fallthrough
    /w14242   # possible data loss
    /w14254   # operator precedence
    /w14263   # override
    /w14265   # virtual dtor
    /w14287   # signed/unsigned mix
    /w14296   # always true/false
  )
  if (ASSISI_WARNINGS_AS_ERRORS)
    target_compile_options(Assisi-Warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(Assisi-Warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wundef
    -Wdouble-promotion
    -Wformat=2
  )
  if (ASSISI_WARNINGS_AS_ERRORS)
    target_compile_options(Assisi-Warnings INTERFACE -Werror)
  endif()
endif()

# ---- Perf knobs: enabled for Release via generator expressions
if (ASSISI_ENABLE_FAST_MATH)
  if (MSVC)
    target_compile_options(Assisi-Perf INTERFACE
      $<$<CONFIG:Release>:/fp:fast>
    )
  else()
    target_compile_options(Assisi-Perf INTERFACE
      $<$<CONFIG:Release>:-ffast-math>
    )
  endif()
endif()

# ---- Sanitizers: enabled only when ASSISI_ENABLE_SANITIZERS=ON (via presets)
if (ASSISI_ENABLE_SANITIZERS)
  if (MSVC)
    target_compile_options(Assisi-Sanitize INTERFACE /fsanitize=address /Zi)
    target_link_options(Assisi-Sanitize INTERFACE /fsanitize=address)
  else()
    string(REPLACE ";" "," _assisi_san_list "${ASSISI_SANITIZER_SET}")
    target_compile_options(Assisi-Sanitize INTERFACE
      -fsanitize=${_assisi_san_list}
      -fno-omit-frame-pointer
      -g
    )
    target_link_options(Assisi-Sanitize INTERFACE
      -fsanitize=${_assisi_san_list}
    )
  endif()
endif()

# ------------------------------------------------------------
# Dependencies (Conan CMakeDeps + CMakeToolchain)
# ------------------------------------------------------------
# Assumes you run:
#   conan install ... -of <buildDir> -g CMakeDeps -g CMakeToolchain
# and then configure with CMAKE_TOOLCHAIN_FILE pointing at <buildDir>/conan_toolchain.cmake
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(stb CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)

add_library(Assisi-Deps INTERFACE)
add_library(Assisi::Deps ALIAS Assisi-Deps)

# Conan target names vary by recipe; prefer modern imported targets but fall back gracefully.
if (TARGET glfw::glfw)
  target_link_libraries(Assisi-Deps INTERFACE glfw::glfw)
elseif (TARGET glfw)
  target_link_libraries(Assisi-Deps INTERFACE glfw)
elseif (TARGET glfw3::glfw3)
  target_link_libraries(Assisi-Deps INTERFACE glfw3::glfw3)
elseif (TARGET glfw3)
  target_link_libraries(Assisi-Deps INTERFACE glfw3)
else()
  message(FATAL_ERROR "glfw3 package found, but no known GLFW target was exported (tried glfw::glfw, glfw, glfw3::glfw3, glfw3).")
endif()


if (TARGET glm::glm)
  target_link_libraries(Assisi-Deps INTERFACE glm::glm)
elseif (TARGET glm)
  target_link_libraries(Assisi-Deps INTERFACE glm)
else()
  message(FATAL_ERROR "glm target not found (expected glm::glm or glm).")
endif()

if (TARGET stb::stb)
  target_link_libraries(Assisi-Deps INTERFACE stb::stb)
elseif (TARGET stb)
  target_link_libraries(Assisi-Deps INTERFACE stb)
else()
  message(FATAL_ERROR "stb target not found (expected stb::stb or stb).")
endif()

if (TARGET glad::glad)
  target_link_libraries(Assisi-Deps INTERFACE glad::glad)
elseif (TARGET glad)
  target_link_libraries(Assisi-Deps INTERFACE glad)
else()
  message(FATAL_ERROR "glad target not found (expected glad::glad or glad).")
endif()

if (TARGET assimp::assimp)
  target_link_libraries(Assisi-Deps INTERFACE assimp::assimp)
elseif (TARGET assimp)
  target_link_libraries(Assisi-Deps INTERFACE assimp)
else()
  message(FATAL_ERROR "assimp target not found (expected assimp::assimp or assimp).")
endif()

# ------------------------------------------------------------
# Project modules / apps (based on your existing layout)
# ------------------------------------------------------------
add_subdirectory(modules/Core)
add_subdirectory(modules/Math)
add_subdirectory(modules/Window)
add_subdirectory(modules/Render)
add_subdirectory(modules/ECS)
add_subdirectory(modules/Runtime)

add_subdirectory(apps/Sandbox)


# Notes for your module CMakeLists:
# - Each module target should link:
#     Assisi::Options, Assisi::Warnings, Assisi::Perf, Assisi::Deps
#   and optionally Assisi::Sanitize (it is inert unless enabled by preset).
