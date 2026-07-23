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

# Chiara, the performance/memory capture system (docs/chiara-design-notes.md).
# OFF on every configuration — debug, dev and ship alike — because capture is a
# decision independent of optimization level, and the build worth profiling is
# the optimized one. The `-chiara` presets and the `-c` make targets (gd-c,
# gs-c, …) turn it on; nothing else does.
option(ASSISI_ENABLE_CHIARA "Compile the Chiara capture system in" OFF)

# Debug-utils labels on the command list, so RenderDoc/Nsight show the engine's
# own pass names instead of a flat list of anonymous draws (Render/GpuMarker.hpp).
# Defaults to following Chiara, since the `-chiara` presets are the builds anyone
# profiles — but it is its own option because the two are independent: a plain
# Release build handed to RenderDoc still wants labels.
option(ASSISI_ENABLE_GPU_MARKERS "Emit GPU debug-utils markers for RenderDoc/Nsight" ${ASSISI_ENABLE_CHIARA})

# Networking (Assisi::Net / Assisi::NetSync) pulls in GameNetworkingSockets and,
# through it, protobuf + abseil — by far the heaviest configure in the tree. It is
# ON by default because networking is a first-class engine subsystem, not an
# optional backend; the switch exists so a contributor who is only touching, say,
# ECS can configure a build that neither fetches nor compiles any of it.
option(ASSISI_ENABLE_NETWORKING "Build the networking modules (fetches GameNetworkingSockets + protobuf)" ON)

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

# Same as Assisi_apply_defaults, plus the handful of warnings that only doctest's
# macros produce. Use it for test executables instead of the plain version.
#
# Two diagnostics fire on essentially every assertion we write, from inside
# doctest rather than from our logic: TEST_CASE expands __COUNTER__ (a C2y
# extension under clang), doctest::Approx takes a double so comparing a float
# against Approx(1.f) promotes, and doctest pulls in <ciso646>, which libstdc++
# 16 #warns is non-standard since C++20. Marking doctest's headers SYSTEM does not
# help — the diagnostic is attributed to the expansion site, i.e. our .cpp.
#
# Applied per-source rather than to the target: Assisi::Warnings arrives as
# INTERFACE options, which land after a target's own options, so a target-level
# -Wno- would be re-enabled by the later -Wextra / -Wdouble-promotion.
function(Assisi_apply_test_defaults target_name)
  Assisi_apply_defaults("${target_name}")

  # Clang-only: gcc does not diagnose either case, so it needs no opt-out and
  # would only warn about an unrecognized option.
  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    get_target_property(_test_sources "${target_name}" SOURCES)
    if (_test_sources)
      set_source_files_properties(${_test_sources}
        TARGET_DIRECTORY "${target_name}"
        PROPERTIES COMPILE_OPTIONS "-Wno-c2y-extensions;-Wno-double-promotion;-Wno-#warnings")
    endif()
  endif()
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
  # Jolt Physics is compiled with AVX2/F16C/FMA/LZCNT/TZCNT; MSVC enables
  # these implicitly on x64, but GCC/Clang require explicit -m flags. x86-only
  # flags — on other architectures (aarch64: NEON via Jolt's own detection)
  # they don't exist and would fail the configure. This sets the engine's x86
  # CPU baseline to Haswell (2013): older CPUs get SIGILL, by design.
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i[3-6]86)$")
    target_compile_options(Assisi-Options INTERFACE
      -mavx2
      -mf16c
      -mfma
      -mlzcnt
      -mbmi   # provides TZCNT
    )
  endif()
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

# ---- Perf knobs: enabled for optimized configs via generator expressions.
# Fast-math applies to dev (RelWithDebInfo) AND ship (Release) so both have
# the same FP semantics — a Release-only flag meant NaN bugs reproduced in dev
# but not ship and vice versa. Debug stays strict FP (it's also the sanitizer
# config). -fno-finite-math-only comes after -ffast-math (last flag wins) to
# opt back out of the NaN/Inf-never-happen assumption: the engine's non-finite
# guards (save-file validation, LightingSystem::SafeDirection) rely on
# isnan/isfinite, which finite-math-only folds to constant false — the guards
# would be dead code exactly in the configs that need them. The networking
# design assumes fast-math stays on (a reason lockstep was rejected), so the
# flag must not be dropped silently.
if (ASSISI_ENABLE_FAST_MATH)
  if (MSVC)
    # /fp:fast does not license MSVC to fold isnan/isfinite to false, so no
    # finite-math opt-out is needed.
    target_compile_options(Assisi-Perf INTERFACE
      $<$<CONFIG:Release,RelWithDebInfo>:/fp:fast>
    )
  else()
    target_compile_options(Assisi-Perf INTERFACE
      $<$<CONFIG:Release,RelWithDebInfo>:-ffast-math;-fno-finite-math-only>
    )
  endif()
endif()

# ---- Dev-only: the source asset tree, so a running editor can mirror newly
# minted asset-id sidecars back out of the disposable staged copy it reads from
# (see AssetSystem::SetAuthoringRoot). Deliberately absent in Release: a shipped
# build has no source tree, and its staged assets are the durable copy.
target_compile_definitions(Assisi-Options INTERFACE
  $<$<NOT:$<CONFIG:Release>>:ASSISI_SOURCE_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets">
)

# ---- Sanitizers: enabled only when ASSISI_ENABLE_SANITIZERS=ON (via presets)
if (ASSISI_ENABLE_SANITIZERS)
  if (MSVC)
    target_compile_options(Assisi-Sanitize INTERFACE /fsanitize=address /Zi)
    target_link_options(Assisi-Sanitize INTERFACE /fsanitize=address)
  else()
    string(REPLACE ";" "," _assisi_san_list "${ASSISI_SANITIZER_SET}")
    set(_assisi_san_extra "")
    if (ASSISI_SANITIZER_SET MATCHES "undefined|vptr")
      # UBSan's vptr check needs typeinfo for every polymorphic type it sees,
      # but Jolt builds itself with -fno-rtti (its Build/CMakeLists.txt), so
      # anything touching JPH:: classes fails to link with "undefined
      # reference to typeinfo for JPH::Shape". Dropping vptr is the documented
      # remedy for linking against non-RTTI code; the rest of UBSan still
      # applies. Revisit only if Jolt gains an RTTI build option we want.
      list(APPEND _assisi_san_extra -fno-sanitize=vptr)
    endif()
    target_compile_options(Assisi-Sanitize INTERFACE
      -fsanitize=${_assisi_san_list}
      ${_assisi_san_extra}
      -fno-omit-frame-pointer
      -g
    )
    target_link_options(Assisi-Sanitize INTERFACE
      -fsanitize=${_assisi_san_list}
      ${_assisi_san_extra}
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

# --- libwebp (WebP decode, incl. animated frames via the demux/AnimDecoder API)
# stb_image can't read WebP, so the loading-spinner's animated .webp is decoded
# here instead. We only need to *decode*, so disable every encoder/tool/mux and
# the JS/extras targets — libwebp still builds the `webp` + `webpdemux` static
# libs that WebPAnimDecoder lives in. Marked SYSTEM so its headers don't trip our
# strict warnings. libwebp ships a proper root CMakeLists, so MakeAvailable
# add_subdirectory()s it and the targets below become linkable.
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP   OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP   OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO   OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX    OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS     OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBP_JS    OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_FUZZTEST   OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libwebp
    GIT_REPOSITORY https://github.com/webmproject/libwebp.git
    GIT_TAG        v1.4.0
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(libwebp)

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

# --- FreeType (font rasterizer backing IMGUI_ENABLE_FREETYPE)
# Fetched rather than find_package'd: this project is fully self-contained via
# FetchContent (no vcpkg/Conan), so there is no system FreeType to find on a
# clean Windows tree. Disable every optional external dependency so FreeType
# builds standalone and never picks up a stray system zlib/png or collides with
# assimp's bundled zlib target. FreeType's own CMake defines the
# Freetype::Freetype alias when built as a subproject.
set(FT_DISABLE_ZLIB     ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON CACHE BOOL "" FORCE)
FetchContent_Declare(freetype
    GIT_REPOSITORY https://github.com/freetype/freetype.git
    GIT_TAG        VER-2-13-3
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(freetype)
# FreeType's in-tree build defines the target "freetype"; the namespaced
# Freetype::Freetype alias only comes from its installed find_package config.
# Provide the alias ourselves so downstream links are backend-agnostic (same
# pattern as assimp above).
if (NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

add_library(Assisi-ImGui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    # FreeType font rasterizer: proper hinting/anti-aliasing, so UI text is crisp
    # rather than the soft look stb_truetype gives (especially at editor sizes).
    "${imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp"
)
target_include_directories(Assisi-ImGui SYSTEM PUBLIC "${imgui_SOURCE_DIR}")
# IMGUI_ENABLE_FREETYPE makes ImGui build its font atlas through FreeType by
# default (PUBLIC so any TU that includes imgui.h sees the FreeType-gated APIs).
# Freetype::Freetype comes from the FetchContent block above, not find_package.
target_compile_definitions(Assisi-ImGui PUBLIC IMGUI_ENABLE_FREETYPE)
target_link_libraries(Assisi-ImGui PUBLIC Freetype::Freetype)
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
    # Bare commit pin, so NO GIT_SHALLOW: a depth-1 fetch only grabs the branch
    # tip and can't reach an arbitrary older commit (see the ImGui note above).
    # No DOWNLOAD_ONLY: it is not a FetchContent_Declare keyword — it leaks into
    # the underlying ExternalProject_Add git-clone args and corrupts them, which
    # silently skips the clone (see the stb/ImGui notes above). ImPlot ships no
    # root CMakeLists, so MakeAvailable just populates it (no add_subdirectory).
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

# --- ImGuizmo (transform manipulator gizmo built on Dear ImGui)
# Unlike ImGui/ImPlot, ImGuizmo ships its own CMakeLists — which builds a duplicate
# imgui target and its example app. We only want the source, so stop MakeAvailable
# from add_subdirectory'ing it by pointing SOURCE_SUBDIR at a directory that holds no
# CMakeLists (the documented populate-only trick — DOWNLOAD_ONLY is not a real
# FetchContent keyword). Then compile the one source we need into our own static lib
# against imgui::imgui (ImGuizmo.cpp needs imgui_internal.h). No release is tagged
# compatible with ImGui 1.92, so pin a specific master commit.
FetchContent_Declare(imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG        fd138a7ce089cfcbfee16da8e8692f37a0e9481e
    # Bare commit pin, so NO GIT_SHALLOW: a depth-1 fetch only grabs the branch
    # tip and can't reach this older commit (see the ImGui note above).
    SOURCE_SUBDIR  do-not-configure-imguizmo
)
FetchContent_MakeAvailable(imguizmo)

add_library(Assisi-ImGuizmo STATIC
    "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp"
)
target_include_directories(Assisi-ImGuizmo SYSTEM PUBLIC "${imguizmo_SOURCE_DIR}/src")
target_link_libraries(Assisi-ImGuizmo PUBLIC imgui::imgui)
# Third-party, not written against our strict warning flags — leave defaults off.
add_library(imguizmo::imguizmo ALIAS Assisi-ImGuizmo)

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

# glslang's own sources emit array-bounds and maybe-uninitialized warnings under
# GCC at -O2/-O3 (they do not appear in Debug). They are upstream's code, not
# ours, and nothing here can act on them — so silence the dependency's targets
# rather than let a ship build report a dozen warnings that no one will ever fix.
# Only the targets glslang actually defines; guarded so a version bump that drops
# one does not fail the configure.
foreach(_glslang_target
        glslang SPIRV MachineIndependent GenericCodeGen OSDependent
        glslang-default-resource-limits glslang-standalone)
    if (TARGET "${_glslang_target}")
        target_compile_options("${_glslang_target}" PRIVATE
            "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")
    endif()
endforeach()

# --- GameNetworkingSockets (+ protobuf, + abseil) — the networking transport.
#
# This is the one dependency in the tree with a dependency of its own that we
# also have to configure, so it is the longest block here. Read top to bottom:
# protobuf first (GNS find_package()s it), then the generator shim, then GNS.
#
# Everything is gated on ASSISI_ENABLE_NETWORKING so a build that does not need
# the network modules never pays the (large) protobuf+abseil compile.
if (ASSISI_ENABLE_NETWORKING)

    # ---- protobuf (GNS's wire format for its own handshake/control messages)
    #
    # Pinned to the current release line rather than the last pre-abseil release
    # (21.12, Dec 2022). 21.x is years out of support — no security backports for
    # a parser that eats network bytes — and old protobuf fights modern GCC/MSVC.
    # The cost is abseil: protobuf's own cmake/abseil-cpp.cmake FetchContent's it
    # (pinned by protobuf's cmake/dependencies.cmake) when no system absl is
    # found, which is what we want — one fewer version pair for us to keep in
    # lockstep.
    #
    # OVERRIDE_FIND_PACKAGE is the load-bearing keyword: GNS's src/CMakeLists.txt
    # calls find_package(Protobuf CONFIG), which would otherwise resolve to the
    # *system* protobuf (or fail on a clean machine) instead of the tree we just
    # built. With the override, that find_package() is redirected here.
    set(protobuf_BUILD_TESTS          OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_EXAMPLES       OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_CONFORMANCE    OFF CACHE BOOL "" FORCE)
    set(protobuf_WITH_ZLIB            OFF CACHE BOOL "" FORCE)
    # protoc is a build-time tool: GNS runs it to generate its .pb.cc sources.
    # libupb has to stay ON with it: protoc links the libupb target, and with the
    # target absent CMake does not error — it degrades the dependency to a plain
    # library name and the link fails late with "cannot find -llibupb".
    set(protobuf_BUILD_PROTOC_BINARIES ON CACHE BOOL "" FORCE)
    set(protobuf_BUILD_LIBUPB          ON CACHE BOOL "" FORCE)
    # Fetch abseil rather than link the distro's. protobuf will happily
    # find_package(absl CONFIG) and pick up e.g. Arch's shared abseil, which
    # silently couples our build to whatever version that machine ships and
    # gives the final binary a runtime dependency on it. This tree is
    # self-contained by design; make protobuf's dependency resolution match.
    set(protobuf_FORCE_FETCH_DEPENDENCIES ON CACHE BOOL "" FORCE)
    # protobuf_INSTALL / ABSL_ENABLE_INSTALL are ON *against* our instincts: we
    # never run `cmake --install`, and this tree installs nothing. But GNS's own
    # CMakeLists unconditionally does install(TARGETS ... EXPORT ...) on its
    # static lib, and CMake refuses to generate an export set whose members link
    # a target ("libprotobuf") that is in no export set. There is no GNS option
    # to suppress its install rules, so the fix is to let protobuf (and, through
    # it, abseil) be exportable too. The cost is install rules nobody invokes.
    # ...and the same for the two dependencies protobuf itself exports through.
    # utf8_range in particular derives its own install flag from protobuf_INSTALL
    # with a non-FORCE cache set, so a build directory configured once with
    # installs off keeps the stale OFF forever — force all three here.
    set(protobuf_INSTALL          ON  CACHE BOOL "" FORCE)
    set(ABSL_ENABLE_INSTALL       ON  CACHE BOOL "" FORCE)
    set(utf8_range_ENABLE_INSTALL ON  CACHE BOOL "" FORCE)
    set(utf8_range_ENABLE_TESTS   OFF CACHE BOOL "" FORCE)
    # abseil: no tests, and it must inherit our C++ standard rather than pinning
    # its own (a mismatch is an ODR/ABI trap, not a warning).
    set(ABSL_PROPAGATE_CXX_STD ON  CACHE BOOL "" FORCE)
    set(ABSL_BUILD_TESTING     OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(protobuf
        GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
        GIT_TAG        v35.1
        GIT_SHALLOW    TRUE
        SYSTEM
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(protobuf)

    # protobuf_generate_cpp() lives in protobuf's *installed* config package
    # (cmake/protobuf-module.cmake.in), which a FetchContent/add_subdirectory
    # consumer never gets — the source tree only defines the modern
    # protobuf_generate(). GNS calls the legacy spelling, so provide it: include
    # the generator that does exist, then a thin forwarding shim. This is exactly
    # what protobuf's own module-compat shim does, minus the imported-target
    # plumbing that is meaningless for an in-tree build.
    if (NOT COMMAND protobuf_generate)
        include("${protobuf_SOURCE_DIR}/cmake/protobuf-generate.cmake")
    endif()
    if (NOT COMMAND protobuf_generate_cpp)
        # protobuf's shim defaults this to TRUE: pass -I for each .proto's own
        # directory, which is what GNS's relative proto paths assume.
        if (NOT DEFINED PROTOBUF_GENERATE_CPP_APPEND_PATH)
            set(PROTOBUF_GENERATE_CPP_APPEND_PATH TRUE)
        endif()
        function(protobuf_generate_cpp SRCS HDRS)
            cmake_parse_arguments(_pgc "" "EXPORT_MACRO" "" ${ARGN})
            if (NOT _pgc_UNPARSED_ARGUMENTS)
                message(SEND_ERROR "protobuf_generate_cpp() called without any .proto files")
                return()
            endif()
            if (PROTOBUF_GENERATE_CPP_APPEND_PATH)
                set(_append_arg APPEND_PATH)
            endif()
            set(_outvar)
            protobuf_generate(${_append_arg} LANGUAGE cpp
                EXPORT_MACRO ${_pgc_EXPORT_MACRO} OUT_VAR _outvar
                PROTOS ${_pgc_UNPARSED_ARGUMENTS})
            set(${SRCS})
            set(${HDRS})
            foreach(_file ${_outvar})
                if (_file MATCHES "cc$")
                    list(APPEND ${SRCS} ${_file})
                else()
                    list(APPEND ${HDRS} ${_file})
                endif()
            endforeach()
            set(${SRCS} ${${SRCS}} PARENT_SCOPE)
            set(${HDRS} ${${HDRS}} PARENT_SCOPE)
        endfunction()
    endif()

    # ---- GameNetworkingSockets itself
    #
    # Pinned to a `master` commit, NOT the newest tag: v1.6.0 is 2021-06-03 and
    # five years stale, while master is actively maintained by Valve (this pin is
    # 2026-06-06) and carries the modern-protobuf compatibility and native-ICE
    # work the tag predates. Record the SHA + date on any bump.
    #
    # Crypto backend is per-platform and adds no fetched dependency:
    #   Windows — AES via the OS (BCrypt), 25519 via GNS's bundled reference impl.
    #   Linux   — system OpenSSL. A deliberate, documented exception to this
    #             tree's "fully self-contained" rule, same class as GLFW's system
    #             X11 libs; OpenSSL is universally present on Linux dev machines.
    #             (Escape hatch if that ever chafes: USE_CRYPTO=libsodium.)
    # ENABLE_ICE stays at its default ON: the native ICE client is in-tree and
    # pulls in nothing extra (USE_STEAMWEBRTC, which *would* drag in Google's
    # WebRTC, stays off), so the Stage-7 NAT-traversal path costs a rebuild of
    # nothing when we get to it.
    set(BUILD_STATIC_LIB         ON  CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIB         OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES           OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS              OFF CACHE BOOL "" FORCE)
    set(BUILD_TOOLS              OFF CACHE BOOL "" FORCE)
    set(USE_STEAMWEBRTC          OFF CACHE BOOL "" FORCE)
    set(Protobuf_USE_STATIC_LIBS ON  CACHE BOOL "" FORCE)
    if (WIN32)
        set(USE_CRYPTO      "BCrypt"    CACHE STRING "" FORCE)
        set(USE_CRYPTO25519 "Reference" CACHE STRING "" FORCE)
    else()
        set(USE_CRYPTO      "OpenSSL" CACHE STRING "" FORCE)
        set(USE_CRYPTO25519 "OpenSSL" CACHE STRING "" FORCE)
    endif()
    FetchContent_Declare(GameNetworkingSockets
        GIT_REPOSITORY https://github.com/ValveSoftware/GameNetworkingSockets.git
        GIT_TAG        f4525e39ee10f6b45181fd92d01fee75f7d71756 # master @ 2026-06-06; newest tag (v1.6.0) is 2021 and too old for current protobuf
        SYSTEM
    )
    FetchContent_MakeAvailable(GameNetworkingSockets)

endif()

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
# Chiara sits *below* Core and is linked PUBLIC by it. Core's JobSystem is the
# only place worker-thread identity exists, so it has to register those threads
# with the capture — which forces the dependency this way round, not the other.
add_subdirectory(modules/Chiara)
add_subdirectory(modules/Core)
add_subdirectory(modules/Math)
# Geometry sits above Core+Math and below Render: CPU-side mesh data and the
# mesh-file importer (glTF via fastgltf), with no GPU dependency.
add_subdirectory(modules/Geometry)
add_subdirectory(modules/Window)
add_subdirectory(modules/Render)
# Net sits low, beside Core: a transport with no ECS/Render dependency, so
# anything (asset streaming, telemetry, editor collaboration) may link it later
# without dragging the world in. The replication layer that *does* know about
# entities is a separate module above ECS.
if (ASSISI_ENABLE_NETWORKING)
  add_subdirectory(modules/Net)
endif()
add_subdirectory(modules/ECS)
add_subdirectory(modules/Runtime)
add_subdirectory(modules/Physics)
add_subdirectory(modules/Debug)
add_subdirectory(modules/App)
add_subdirectory(modules/Editor)

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
