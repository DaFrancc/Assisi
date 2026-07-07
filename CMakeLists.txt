cmake_minimum_required(VERSION 3.28)

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
# Central interface targets
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
    # Jolt Physics is compiled with AVX2/F16C/FMA/LZCNT/TZCNT; MSVC enables
    # these implicitly on x64, but GCC/Clang require explicit -m flags.
    -mavx2
    -mf16c
    -mfma
    -mlzcnt
    -mbmi   # provides TZCNT
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
# Dependencies (FetchContent — no Conan required)
# ------------------------------------------------------------
include(FetchContent)

# Share git clones across all presets so sources are only downloaded once.
# Build artifacts still live per-preset in ${CMAKE_BINARY_DIR}/_deps/.
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/out/deps" CACHE PATH "")
# Skip remote update checks on subsequent configures (fast reconfigure).
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# --- GLFW
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw3
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glfw3)

# --- GLM (header-only)
set(GLM_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glm)

# --- stb (header-only; no CMake build)
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
    DOWNLOAD_ONLY  YES
)
FetchContent_MakeAvailable(stb)
add_library(stb-headers INTERFACE)
target_include_directories(stb-headers INTERFACE "${stb_SOURCE_DIR}")
add_library(stb::stb ALIAS stb-headers)

# --- glad (vendored, pre-generated for OpenGL 4.6 core)
add_library(glad_gl STATIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/glad/src/glad.c")
target_include_directories(glad_gl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/glad/include")
add_library(glad::glad ALIAS glad_gl)

# --- Assimp
set(ASSIMP_BUILD_TESTS        OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL            OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT          ON  CACHE BOOL "" FORCE)
FetchContent_Declare(assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG        v5.4.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(assimp)
if (NOT TARGET assimp::assimp)
    add_library(assimp::assimp ALIAS assimp)
endif()

# --- Jolt Physics
set(FLOATING_POINT_EXCEPTIONS_ENABLED    OFF CACHE BOOL "" FORCE)
set(USE_ASSERTS                          OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY      OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS                    OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD                OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST           OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES                    OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER                     OFF CACHE BOOL "" FORCE)
FetchContent_Declare(JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG        v5.2.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  Build
)
FetchContent_MakeAvailable(JoltPhysics)
# Jolt v5.2.0 enables /WX; newer Windows SDK (10.0.26100+) emits a warning in
# wingdi.h that would fail the build. Downgrade to warning-only for Jolt only.
if (MSVC)
    target_compile_options(Jolt PRIVATE /WX-)
endif()
if (NOT TARGET Jolt::Jolt)
    add_library(Jolt::Jolt ALIAS Jolt)
endif()

# --- nlohmann/json
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# --- Dear ImGui (docking branch)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
    DOWNLOAD_ONLY  YES
)
FetchContent_MakeAvailable(imgui)

add_library(Assisi-ImGui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
)
target_include_directories(Assisi-ImGui PUBLIC "${imgui_SOURCE_DIR}")
# ImGui doesn't use our strict warning flags, so don't apply Assisi defaults
add_library(imgui::imgui ALIAS Assisi-ImGui)

set(ASSISI_IMGUI_ROOT "${imgui_SOURCE_DIR}")

# --- NVRHI (Vulkan backend only; DX11/DX12 disabled — Windows+Linux via Vulkan)
set(NVRHI_WITH_VULKAN     ON  CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX11       OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12       OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_VALIDATION ON  CACHE BOOL "" FORCE)
set(NVRHI_INSTALL         OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nvrhi
    GIT_REPOSITORY https://github.com/NVIDIA-RTX/NVRHI.git
    GIT_TAG        8e8c36e37558acec333204619b95d9d2fcdc4a79 # NVRHI has no tagged releases; pinned to a specific commit
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nvrhi)

# --- glslang (GLSL -> SPIR-V compiler, used as a build-time tool only)
set(ENABLE_OPT              OFF CACHE BOOL "" FORCE) # avoids needing SPIRV-Tools-opt / update_glslang_sources.py
set(ENABLE_HLSL             OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES ON  CACHE BOOL "" FORCE) # builds the glslang-standalone compiler executable
set(GLSLANG_TESTS           OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG        vulkan-sdk-1.4.350.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glslang)

# --- Assisi-Deps: aggregate third-party deps into one interface target
add_library(Assisi-Deps INTERFACE)
add_library(Assisi::Deps ALIAS Assisi-Deps)
target_link_libraries(Assisi-Deps INTERFACE
    glfw
    glm::glm
    stb::stb
    glad::glad
    assimp::assimp
    imgui::imgui
)

# ------------------------------------------------------------
# Tooling
# ------------------------------------------------------------
include(cmake/AssisiReflect.cmake)

# ------------------------------------------------------------
# Project modules / apps (based on your existing layout)
# ------------------------------------------------------------
add_subdirectory(modules/Core)
add_subdirectory(modules/Math)
add_subdirectory(modules/Window)
add_subdirectory(modules/Render)
add_subdirectory(modules/ECS)
add_subdirectory(modules/Runtime)
add_subdirectory(modules/Physics)
add_subdirectory(modules/Debug)
add_subdirectory(modules/App)

# CMake 4.x scans each binary subdir for CMakePresets.json immediately after
# creating it, before the subdirectory's CMakeLists.txt is processed. Pre-create
# empty stubs so the scan succeeds. Must be done before add_subdirectory.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/apps/sandbox")
file(WRITE "${CMAKE_BINARY_DIR}/apps/sandbox/CMakePresets.json" "{\"version\":10}")
add_subdirectory(apps/sandbox)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/apps/vk_triangle")
file(WRITE "${CMAKE_BINARY_DIR}/apps/vk_triangle/CMakePresets.json" "{\"version\":10}")
add_subdirectory(apps/vk_triangle)


# Notes for module CMakeLists:
# - Each module target should link:
#     Assisi::Options, Assisi::Warnings, Assisi::Perf, Assisi::Deps
#   and optionally Assisi::Sanitize (it is inert unless enabled by preset).

#add_subdirectory(apps/CustomAppName)
