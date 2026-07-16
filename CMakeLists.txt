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

# Unit tests (pure-logic modules: ECS, asset-path, action-map, topo-sort).
option(ASSISI_BUILD_TESTS "Build unit tests" ON)

# Assimp is the deferred multi-format catch-all mesh-import backend (FBX/OBJ/DAE/…).
# The runtime glTF path uses fastgltf; Assimp stays OFF so its heavy build doesn't
# tax every configure until a second format backend is actually wired. Flip this ON
# to fetch and build it (a future Geometry Assimp backend would then link it).
option(ASSISI_ENABLE_ASSIMP "Build the Assimp multi-format mesh-import backend" OFF)

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

# Keep third-party dependency configuration quiet. FetchContent pulls in a lot
# of vendored CMake we don't own and can't clean up: glm's pre-3.10
# cmake_minimum_required, assimp's CMP0175 add_custom_command usage, and pages of
# message(STATUS) chatter (importer lists, "compiling zlib", version banners).
# All of it buries our own build output, so we hush it for the duration of the
# FetchContent block and restore normal verbosity afterwards. Two companion
# pieces complete the muffling: -Wno-dev / -Wno-deprecated live in CMakePresets'
# base preset (they silence the CMake author/deprecation warnings that a raised
# log level would still print), and each dependency's include directories are
# marked SYSTEM (the SYSTEM keyword on FetchContent_Declare, or SYSTEM on our
# own include dirs) so their headers don't trip our /W4 + -Wconversion warnings
# when we include them.
set(_assisi_saved_message_log_level "${CMAKE_MESSAGE_LOG_LEVEL}")
set(CMAKE_MESSAGE_LOG_LEVEL NOTICE)   # hide dep message(STATUS); keep WARNING/ERROR

# Silence the CMake author/deprecation warnings that vendored dep CMake emits
# (glm's pre-3.10 cmake_minimum_required; assimp's CMP0175 add_custom_command
# usage). These MUST be set here rather than only via CMakePresets' `warnings`
# field: the preset's -Wno-dev/-Wno-deprecated only apply to an explicit
# `cmake --preset` run, but Ninja's auto-reconfigure ("Re-running CMake" on every
# build) regenerates without them, so the preset alone leaves these showing on
# every incremental build. CMAKE_WARN_DEPRECATED is restored after the block, and
# CMP0175 is set as a per-policy *default* (only affects subdirs that don't set
# it) and unset afterwards, so our own add_custom_command calls (e.g. reflectgen)
# are still held to the current-CMake standard.
# CMAKE_WARN_DEPRECATED is read from the cache, so a plain set() is ignored — it
# must be forced in the cache. Save the prior cache state and restore it after the
# block so our own CMake keeps its deprecation diagnostics.
if (DEFINED CACHE{CMAKE_WARN_DEPRECATED})
    set(_assisi_had_warn_deprecated ON)
    set(_assisi_saved_warn_deprecated "$CACHE{CMAKE_WARN_DEPRECATED}")
else()
    set(_assisi_had_warn_deprecated OFF)
endif()
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_DEFAULT_CMP0175 OLD)   # assimp: add_custom_command without PRE/POST_BUILD

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
    SYSTEM
)
FetchContent_MakeAvailable(glfw3)

# --- GLM (header-only)
set(GLM_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(glm)

# --- stb (header-only; no CMake build)
# Pinned to a bare commit, so NO GIT_SHALLOW: a depth-1 fetch can only grab a
# ref tip, and once upstream moves past this commit a shallow clone can no longer
# check it out ("reference is not a tree"). A full clone can reach any commit.
# No DOWNLOAD_ONLY: it is not a FetchContent_Declare keyword — it leaks into the
# underlying ExternalProject_Add git-clone args and corrupts them. stb ships no
# root CMakeLists, so MakeAvailable just populates it (no add_subdirectory) anyway.
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        31c1ad37456438565541f4919958214b6e762fb4 # stb has no tagged releases; pinned to a specific commit
)
FetchContent_MakeAvailable(stb)
add_library(stb-headers INTERFACE)
target_include_directories(stb-headers SYSTEM INTERFACE "${stb_SOURCE_DIR}")
add_library(stb::stb ALIAS stb-headers)

# --- Assimp (deferred catch-all import backend; off by default, see ASSISI_ENABLE_ASSIMP)
if (ASSISI_ENABLE_ASSIMP)
    # Vendor zlib+minizip rather than picking up a system minizip. On distros that
    # ship minizip (e.g. Arch/Manjaro), assimp finds it via pkg-config but its headers
    # live under /usr/include/minizip/, whereas ZipArchiveIOSystem.cpp does
    # #include <unzip.h> unqualified -> "unzip.h: No such file". Forcing the bundled
    # build sets ASSIMP_BUILD_MINIZIP and adds contrib/unzip to the include path.
    set(ASSIMP_BUILD_ZLIB         ON  CACHE BOOL "" FORCE)
    # Assimp compiles its own sources with -Werror; newer GCC (13+) emits warnings in
    # upstream loaders (e.g. unused-but-set-variable in MS3DLoader.cpp) that we can't
    # fix without patching the vendored tree. Don't fail our build on assimp's warnings.
    set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_TESTS        OFF CACHE BOOL "" FORCE)
    set(ASSIMP_INSTALL            OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_NO_EXPORT          ON  CACHE BOOL "" FORCE)
    FetchContent_Declare(assimp
        GIT_REPOSITORY https://github.com/assimp/assimp.git
        GIT_TAG        v5.4.3
        GIT_SHALLOW    TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(assimp)
    if (NOT TARGET assimp::assimp)
        add_library(assimp::assimp ALIAS assimp)
    endif()
endif()

# --- fastgltf (glTF 2.0 loader; the runtime mesh-import backend in Geometry).
# Depends only on simdjson, which its CMake fetches automatically.
set(FASTGLTF_ENABLE_TESTS    OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_DOCS     OFF CACHE BOOL "" FORCE)
FetchContent_Declare(fastgltf
    GIT_REPOSITORY https://github.com/spnda/fastgltf.git
    GIT_TAG        v0.9.0
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(fastgltf)

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
    SYSTEM
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
    SYSTEM
)
FetchContent_MakeAvailable(nlohmann_json)

# --- doctest (unit-test framework; header-only, included as SYSTEM headers)
if (ASSISI_BUILD_TESTS)
    # Point SOURCE_SUBDIR at a nonexistent path so MakeAvailable populates the
    # headers but never add_subdirectory()s doctest's own CMakeLists — that file
    # sets cmake_minimum_required below 3.5, which modern CMake rejects. We only
    # need ${doctest_SOURCE_DIR} for the include path, not its build targets.
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.4.11
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _headers_only_no_build
    )
    FetchContent_MakeAvailable(doctest)
    enable_testing()
endif()

# --- Dear ImGui (docking branch)
# Pinned to a bare docking-branch commit, so NO GIT_SHALLOW: once the docking
# branch advances past this commit a depth-1 fetch stops containing it and the
# checkout fails ("reference is not a tree"). A full clone can reach any commit.
# No DOWNLOAD_ONLY: it is not a FetchContent_Declare keyword — it leaks into the
# underlying ExternalProject_Add git-clone args and corrupts them. ImGui ships no
# root CMakeLists, so MakeAvailable just populates it (no add_subdirectory) anyway.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        4be08b1ecf7709f15e4274fb2ddac37e121d7d9a # docking-branch commit (no tagged docking release); pinned for reproducibility
)
FetchContent_MakeAvailable(imgui)

add_library(Assisi-ImGui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
)
target_include_directories(Assisi-ImGui SYSTEM PUBLIC "${imgui_SOURCE_DIR}")
# ImGui doesn't use our strict warning flags, so don't apply Assisi defaults
add_library(imgui::imgui ALIAS Assisi-ImGui)

set(ASSISI_IMGUI_ROOT "${imgui_SOURCE_DIR}")

# --- ImPlot (plotting widgets built on Dear ImGui)
# Ships no CMake of its own (a source drop like ImGui), so compile it into our
# own static lib against imgui::imgui. Tracks master rather than the v0.16 tag
# because that release predates — and won't build against — ImGui 1.92. No
# post-1.92 release is tagged yet, so pin to a specific master commit.
FetchContent_Declare(implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        d65a2bef53d32502407de3a4be80f191e2f412d7
    GIT_SHALLOW    TRUE
    DOWNLOAD_ONLY  YES
)
FetchContent_MakeAvailable(implot)

add_library(Assisi-ImPlot STATIC
    "${implot_SOURCE_DIR}/implot.cpp"
    "${implot_SOURCE_DIR}/implot_items.cpp"
)
target_include_directories(Assisi-ImPlot SYSTEM PUBLIC "${implot_SOURCE_DIR}")
target_link_libraries(Assisi-ImPlot PUBLIC imgui::imgui)
# Like ImGui, ImPlot isn't written against our strict warning flags — leave defaults off.
add_library(implot::implot ALIAS Assisi-ImPlot)

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
    SYSTEM
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
    SYSTEM
)
FetchContent_MakeAvailable(glslang)

# End of third-party dependencies — restore normal message verbosity and the
# deprecation-warning / policy state so our own configuration output (modules,
# reflectgen, apps) prints and is diagnosed as usual.
set(CMAKE_MESSAGE_LOG_LEVEL "${_assisi_saved_message_log_level}")
if (_assisi_had_warn_deprecated)
    set(CMAKE_WARN_DEPRECATED "${_assisi_saved_warn_deprecated}" CACHE BOOL "" FORCE)
else()
    unset(CMAKE_WARN_DEPRECATED CACHE)
endif()
unset(CMAKE_POLICY_DEFAULT_CMP0175)

# --- Assisi-Deps: aggregate third-party deps into one interface target
add_library(Assisi-Deps INTERFACE)
add_library(Assisi::Deps ALIAS Assisi-Deps)
target_link_libraries(Assisi-Deps INTERFACE
    glfw
    glm::glm
    stb::stb
    imgui::imgui
)

# ------------------------------------------------------------
# Tooling
# ------------------------------------------------------------
include(cmake/AssisiReflect.cmake)

# ------------------------------------------------------------
# Ship builds: strip at link time.
# Our own code compiles without -g, but several vendored static libs (assimp,
# glslang, SPIRV-Tools, ...) bake -g into their objects regardless of our
# Release build type, dragging ~27 MB of DWARF into the final executable. A
# link-time strip drops all of it no matter which object carried it. Gated on
# Release (ship) only, so RelWithDebInfo (dev) and Debug keep their symbols.
# Placed *after* the FetchContent block so only our modules/apps below inherit
# it — the deps' own tool executables (glslang StandAlone, etc.) are untouched.
if (NOT MSVC)
  add_link_options($<$<CONFIG:Release>:-s>)
endif()

# ------------------------------------------------------------
# Project modules / apps (based on your existing layout)
# ------------------------------------------------------------
add_subdirectory(modules/Core)
add_subdirectory(modules/Math)
# Geometry sits above Core+Math and below Render: CPU-side mesh data and the
# mesh-file importer (glTF via fastgltf), with no GPU dependency.
add_subdirectory(modules/Geometry)
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

# Per-module unit tests live in modules/<Name>/tests and are pulled in by each
# module's CMakeLists when ASSISI_BUILD_TESTS is ON (see enable_testing() above).

# reflectgen (the reflection codegen script) has its own Python golden-file tests.
if (ASSISI_BUILD_TESTS)
    add_subdirectory(tools/reflectgen)
endif()

# Notes for module CMakeLists:
# - Each module target should link:
#     Assisi::Options, Assisi::Warnings, Assisi::Perf, Assisi::Deps
#   and optionally Assisi::Sanitize (it is inert unless enabled by preset).

#add_subdirectory(apps/CustomAppName)
