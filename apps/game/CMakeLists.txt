# The project splits in two:
#
#   src/     the game itself — its systems and components. Everything here is
#            picked up by glob, so adding a file to the game is only ever
#            creating it. CONFIGURE_DEPENDS re-runs the glob on every build, so a
#            new file is seen without anyone re-running CMake by hand.
#   launch/  how the game starts: the two main()s, their argument parsing, and
#            the headless server. A game can change these but rarely needs to,
#            so they are listed by name and kept out of the way.
#
# Both are compiled into one static library that both executables link, so the
# two binaries cannot drift in what they contain. The reflection registrations
# deliberately do NOT live here — see the INTERFACE library below for why an
# archive is the wrong place for them.
add_library(Assisi-GameLib STATIC)
Assisi_apply_defaults(Assisi-GameLib)

file(GLOB_RECURSE _game_sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE _game_headers CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

target_sources(Assisi-GameLib
  PRIVATE
    ${_game_sources}
    ${_game_headers}
    "launch/LaunchArgs.cpp"
    "launch/LaunchArgs.hpp"
    "launch/ServerApp.cpp"
    "launch/ServerApp.hpp"
)

target_include_directories(Assisi-GameLib
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/launch"
)

target_link_libraries(Assisi-GameLib
  PUBLIC
    # ServerApp derives from Application directly and never touches the editor.
    Assisi::App
    Assisi::Physics
)

# ServerApp keeps its Offline role without networking — headless simulation is a
# separate thing from replication — so the file stays in the build and guards the
# Host/Client halves internally.
if (ASSISI_ENABLE_NETWORKING)
  target_link_libraries(Assisi-GameLib PUBLIC Assisi::NetSync)
endif()

# The game's own ASYSTEM and ACOMP declarations. Scanning them is what puts them
# in the catalog: a level names a system, and the declaration in a reflected
# header is the registration — there is no registerGameSystems to call.
#
# Every header under src/ is scanned, not a hand-kept list of the annotated
# ones. A header missing from such a list compiles and links fine and simply
# never registers, which is a failure nothing reports. A header with no
# annotations costs one empty generated file, and a src/ with no headers at all
# reflects nothing rather than failing to configure.
#
# Reflected on an INTERFACE lib rather than on Assisi-GameLib, and the generated
# objects are added to each executable rather than to the archive. Both halves
# matter: assisi_reflect links its generated OBJECT library into its target, and
# a static library would let the linker drop those objects as unreferenced —
# which silently discards every registration in them.
add_library(Assisi-Game-Systems INTERFACE)
target_include_directories(Assisi-Game-Systems INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Assisi-Game-Systems INTERFACE Assisi::App)
if (_game_headers)
  assisi_reflect(
    TARGET       Assisi-Game-Systems
    INCLUDE_ROOT src
    HEADERS      ${_game_headers}
  )
endif()

# --- The two executables.
#
# Game links no editor and has no way to reach one; GameEditor is the same game
# with Assisi::Editor on top. The editor is a library rather than its own
# executable because reflection registers per final binary — a separate editor
# process could not inspect the components this project's code declares.
#
# Game is out of the default build: editor iteration is the common case, and a
# second full link of every reflected type is time spent on a binary nobody is
# running. Ask for it by name (`--target Assisi-Game`, or a Makefile `-game`
# target).
add_executable(Assisi-Game EXCLUDE_FROM_ALL)
Assisi_apply_defaults(Assisi-Game)
target_sources(Assisi-Game PRIVATE "launch/GameMain.cpp")
target_link_libraries(Assisi-Game PRIVATE Assisi-GameLib Assisi-Game-Systems)
if (ASSISI_PAK_OVERRIDES)
  target_compile_definitions(Assisi-Game PRIVATE ASSISI_PAK_OVERRIDES)
endif()
# Windows opens a console window for every console-subsystem program it launches,
# so a player double-clicking the game would get one beside it. A GUI-subsystem
# program gets none; main() stays the entry point rather than WinMain, and
# GameMain borrows the terminal's console when there is one to borrow.
if (WIN32)
  set_target_properties(Assisi-Game PROPERTIES WIN32_EXECUTABLE ON)
  if (MSVC)
    target_link_options(Assisi-Game PRIVATE /ENTRY:mainCRTStartup)
  endif()
endif()
# The C++ runtime goes inside the game, so a player needs no libstdc++ or
# libgcc_s of their own — and none at least as new as the build machine's
# compiler. glibc stays shared: a static one cannot load the Vulkan driver or the
# windowing libraries.
#
# The static runtime is a separate distro package (libstdc++-static on Fedora,
# included with the compiler on Debian and Arch). Without it a Release
# configure stops and names it, since that build is the one players receive; any
# other build links the runtime shared and says so, and GameNeedsOnlySystemLibraries
# fails until it is installed.
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT WIN32 AND NOT APPLE)
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_LINK_OPTIONS -static-libstdc++ -static-libgcc)
  check_cxx_source_compiles("#include <string>\nint main() { return static_cast<int>(std::string(\"x\").size()) - 1; }"
                            ASSISI_HAVE_STATIC_LIBSTDCXX)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
  if (ASSISI_HAVE_STATIC_LIBSTDCXX)
    target_link_options(Assisi-Game PRIVATE -static-libstdc++ -static-libgcc)
    # The game links C code (GLFW, libsodium), so CMake adds the C compiler's
    # implicit libraries to a link the C++ driver already completes, and on some
    # toolchains (Fedora's GCC) that list names libgcc_s. Placed ahead of the
    # driver's own static libgcc_eh, it resolves the unwinder and puts libgcc_s
    # back in the dynamic section that -static-libgcc took it out of.
    foreach(_language C ASM)
      list(REMOVE_ITEM CMAKE_${_language}_IMPLICIT_LINK_LIBRARIES gcc_s gcc_s_asneeded)
    endforeach()
  elseif (CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "The game links the C++ runtime statically, and this toolchain has no static "
                        "libstdc++. Install it (on Fedora: libstdc++-static) and configure again.")
  else()
    message(WARNING "No static libstdc++: the game links the C++ runtime shared in this build. "
                    "Install it (on Fedora: libstdc++-static) for a game that runs without one.")
  endif()
endif()

add_executable(Assisi-GameEditor)
Assisi_apply_defaults(Assisi-GameEditor)
target_sources(Assisi-GameEditor PRIVATE "launch/EditorMain.cpp")
target_link_libraries(Assisi-GameEditor PRIVATE Assisi-GameLib Assisi-Game-Systems Assisi::Editor)

# Per-binary, for the reason stated above the INTERFACE library: an executable
# cannot be linked into, so its registrations arrive as objects.
foreach(_exe Assisi-Game Assisi-GameEditor)
  if (TARGET Assisi-Game-Systems-Generated)
    target_sources("${_exe}" PRIVATE $<TARGET_OBJECTS:Assisi-Game-Systems-Generated>)
  endif()
  assisi_link_reflections("${_exe}")
endforeach()

# A refused start has to reach the exit code. Driven as a real process because
# that is the whole claim — `StartupFailed()` returning true is not the property
# anyone depends on, `$?` being non-zero is, and only main() joins the two.
#
# `--connect` at a name the transport will not resolve is the cheapest of the
# five refusal paths to reach: NetTransport::Connect takes IP literals only, so
# this fails inside NetSession::Join, synchronously, during OnStart — no peer to
# stand up, no port to bind, no timeout to wait out, and nothing about it varies
# with the machine. WILL_FAIL asserts the non-zero exit.
if (ASSISI_BUILD_TESTS AND ASSISI_ENABLE_NETWORKING)
  add_test(NAME GameEditorRefusedStart
    COMMAND Assisi-GameEditor --connect not-an-ip:27015 --ticks 1
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:Assisi-GameEditor>"
  )
  set_tests_properties(GameEditorRefusedStart PROPERTIES WILL_FAIL TRUE)
endif()

# The Game target boots, steps, and exits on its own. Headless because a build
# machine has no display, and tick-limited because a windowed run ends when
# somebody closes the window. What it proves is that the no-editor link is a real
# program: it resolves, brings the world up, and returns 0.
if (ASSISI_BUILD_TESTS)
  # Builds Game before anything that runs or scans it. Game is out of the default
  # build, so without this a tree built for the editor would test whatever Game
  # binary an earlier build left behind — passing or failing on code that is no
  # longer the source. A no-op when Game is current. Every test that needs it
  # carries the `game` label, so `ctest -LE game` leaves Game out entirely.
  #
  # The cook and pack tools come with it: every game test reads a package they
  # made, and the pack tool is out of the default build too.
  add_test(NAME GameBuilt
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
            --target Assisi-Game Assisi-Cook-Tool Assisi-Pack-Tool
  )
  set_tests_properties(GameBuilt PROPERTIES
    FIXTURES_SETUP GameBinary
    LABELS game)
endif()

# --- The game reads a package, and the configured startup scene decides whether
#     it starts at all.
#
# Two asset roots written here, each holding a config and nothing else of
# consequence, cooked and packed by the same tools a release uses. Purpose-built
# rather than the repository's own tree: the claim is about what the config
# names, and a package that holds every level would let a refusal pass by finding
# something else to open.
#
# Neither is named "assets", which is the one name DiscoverRoot would find on its
# own by walking up from the editor — a root that shadowed the real tree would
# change what the editor tests read.
if (ASSISI_BUILD_TESTS)
  set(_thin_root "${CMAKE_CURRENT_BINARY_DIR}/thin-root")
  set(_refused_root "${CMAKE_CURRENT_BINARY_DIR}/refused-root")

  # PerfBlank is a camera and nothing else: no meshes, no materials, no
  # blueprints, and an empty system list. A level with content would need that
  # content staged beside it, and a failure to stage it would look exactly like
  # the failure this test is looking for.
  file(COPY "${CMAKE_SOURCE_DIR}/assets/levels/PerfBlank.alvl"
            "${CMAKE_SOURCE_DIR}/assets/levels/PerfBlank.alvl.aast"
       DESTINATION "${_thin_root}/levels")

  # The same config in both roots but for the scene it names, so a difference in
  # outcome can only come from the scene. The sidecar gives the config an id to
  # be cooked under.
  foreach(_root_scene "${_thin_root}|levels/PerfBlank.alvl" "${_refused_root}|levels/Absent.alvl")
    string(REPLACE "|" ";" _root_scene "${_root_scene}")
    list(GET _root_scene 0 _root)
    list(GET _root_scene 1 _scene)
    file(WRITE "${_root}/config/game.json"
      "{\n"
      "  \"version\": 1,\n"
      "  \"type\": \"AppConfig\",\n"
      "  \"startupScene\": \"${_scene}\"\n"
      "}\n")
    file(WRITE "${_root}/config/game.json.aast"
      "{\n"
      "  \"guid\": \"7d3c1f0e-2b4a-4c8d-9e6f-1a2b3c4d5e6f\",\n"
      "  \"type\": \"AssetSidecar\",\n"
      "  \"version\": 1\n"
      "}\n")
  endforeach()

  # Cooked and packed as tests rather than at configure time, because the tools
  # that do it are built by GameBuilt. lz4, so every boot below also decompresses.
  foreach(_name thin refused)
    add_test(NAME Game-${_name}-Cooked
      COMMAND Assisi-Cook-Tool --source "${CMAKE_CURRENT_BINARY_DIR}/${_name}-root"
                               --out "${CMAKE_CURRENT_BINARY_DIR}/${_name}-cooked" --texture-tier fast
    )
    set_tests_properties(Game-${_name}-Cooked PROPERTIES
      FIXTURES_REQUIRED GameBinary
      FIXTURES_SETUP ${_name}Cooked
      LABELS game)

    add_test(NAME Game-${_name}-Packed
      COMMAND Assisi-Pack-Tool --cooked "${CMAKE_CURRENT_BINARY_DIR}/${_name}-cooked"
                               --out "${CMAKE_CURRENT_BINARY_DIR}/${_name}.pak" --compress lz4
    )
    set_tests_properties(Game-${_name}-Packed PROPERTIES
      FIXTURES_REQUIRED "GameBinary;${_name}Cooked"
      FIXTURES_SETUP ${_name}Pak
      LABELS game)
  endforeach()

  # A copy of the game with the thin package beside it under the default name,
  # and one with no package at all. Copies, so these read the path a shipped build
  # reads without the developer's own package beside the real executable getting
  # in the way — and they work whether or not overrides are compiled in.
  set(_staged "${CMAKE_CURRENT_BINARY_DIR}/staged")
  set(_unstaged "${CMAKE_CURRENT_BINARY_DIR}/unstaged")
  set(_stage_script "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageGame.cmake")
  add_test(NAME Game-Staged
    COMMAND "${CMAKE_COMMAND}" "-DGAME=$<TARGET_FILE:Assisi-Game>" "-DDIR=${_staged}"
            "-DPAK=${CMAKE_CURRENT_BINARY_DIR}/thin.pak" -P "${_stage_script}"
  )
  set_tests_properties(Game-Staged PROPERTIES
    FIXTURES_REQUIRED "GameBinary;thinPak"
    FIXTURES_SETUP StagedGame
    LABELS game)
  add_test(NAME Game-Unstaged
    COMMAND "${CMAKE_COMMAND}" "-DGAME=$<TARGET_FILE:Assisi-Game>" "-DDIR=${_unstaged}" -P "${_stage_script}"
  )
  set_tests_properties(Game-Unstaged PROPERTIES
    FIXTURES_REQUIRED GameBinary
    FIXTURES_SETUP UnstagedGame
    LABELS game)

  # The Game target boots, steps, and exits on its own from the package beside it.
  # Headless because a build machine has no display, and tick-limited because a
  # windowed run ends when somebody closes the window. What it proves is that the
  # no-editor link is a real program that reads what a release ships: it resolves,
  # brings the world up, and returns 0. It is also the positive control for every
  # refusal below — without it a refusal passes whenever the game fails for any
  # reason at all.
  add_test(NAME GameBoots
    COMMAND "${_staged}/$<TARGET_FILE_NAME:Assisi-Game>" --headless --ticks 1
    WORKING_DIRECTORY "${_staged}"
  )
  # A game that never reaches its tick limit would otherwise hang the suite
  # rather than fail it. ASSISI_PAK cleared, so a developer's own setting cannot
  # stand in for the default path.
  set_tests_properties(GameBoots PROPERTIES
    ENVIRONMENT "ASSISI_PAK="
    FIXTURES_REQUIRED StagedGame
    LABELS game
    TIMEOUT 60)

  # No package beside the game: it refuses rather than opening an empty world or
  # reading loose files, and the exit code says so.
  add_test(NAME GameRefusesMissingPak
    COMMAND "${_unstaged}/$<TARGET_FILE_NAME:Assisi-Game>" --headless --ticks 1
    WORKING_DIRECTORY "${_unstaged}"
  )
  set_tests_properties(GameRefusesMissingPak PROPERTIES
    ENVIRONMENT "ASSISI_PAK="
    FIXTURES_REQUIRED UnstagedGame
    LABELS game
    WILL_FAIL TRUE
    TIMEOUT 60)

  if (ASSISI_PAK_OVERRIDES)
    # The same boot through --pak, which is how a developer runs a package that is
    # not beside the executable.
    add_test(NAME GameBootsFromNamedPak
      COMMAND Assisi-Game --headless --ticks 1 --pak "${CMAKE_CURRENT_BINARY_DIR}/thin.pak"
      WORKING_DIRECTORY "$<TARGET_FILE_DIR:Assisi-Game>"
    )
    set_tests_properties(GameBootsFromNamedPak PROPERTIES
      FIXTURES_REQUIRED "GameBinary;thinPak"
      LABELS game
      TIMEOUT 60)

    # The claim a startup scene carries: one that is not in the package stops the
    # game and reaches the exit code. Driven as a real process because that is the
    # whole property — a refusal that logs and exits 0 is indistinguishable to
    # anything that launches this from a game the player quit.
    add_test(NAME GameRefusesMissingStartupScene
      COMMAND Assisi-Game --headless --ticks 1
      WORKING_DIRECTORY "$<TARGET_FILE_DIR:Assisi-Game>"
    )
    set_tests_properties(GameRefusesMissingStartupScene PROPERTIES
      ENVIRONMENT "ASSISI_PAK=${CMAKE_CURRENT_BINARY_DIR}/refused.pak"
      FIXTURES_REQUIRED "GameBinary;refusedPak"
      LABELS game
      WILL_FAIL TRUE
      TIMEOUT 60)
  endif()
endif()

# The same exclusion as the symbol scans below, asserted against the link graph
# instead of the built binary — so it holds on the presets a scan cannot read: a
# Release link strips the symbol table away, and MSVC has no nm. It also fails
# at configure time, before anything is compiled.
include("${CMAKE_SOURCE_DIR}/cmake/AssisiLinkGuard.cmake")
assisi_assert_link_excludes(Assisi-Game
    Assisi-Editor
    Assisi::Editor
    Assisi-Debug
    Assisi::Debug
    Assisi-ImGui
    imgui::imgui
    implot::implot
    imguizmo::imguizmo
    Assisi-Cook
    Assisi::Cook
    Assisi-GeometryImport
    Assisi::GeometryImport
    Assisi-ImageImport
    Assisi::ImageImport
    fastgltf::fastgltf
    bc7enc::bc7enc
)

# --- What the Game binary is not allowed to contain.
#
# The editor is kept out by not linking it, and these are what prove the link
# stayed that way. A runtime flag would be checkable by reading the code; a link
# is only checkable by looking at what came out, because the way this regresses
# is somebody adding an include that drags a library back in, which changes no
# behaviour anyone would notice until a player unpacks a level editor.
#
# The scanner lives in scripts/ and is exercised by its own test below: the
# failure that matters is a scan which cannot run reporting success, and that
# case cannot be reached from here because it needs an nm that does not work.
if (ASSISI_BUILD_TESTS)
  set(_symbol_scanner "${CMAKE_SOURCE_DIR}/scripts/check-symbols.py")

  # Must equal SKIP_EXIT_CODE in check-symbols.py. A scan that could not run is
  # neither a pass nor a failure, and calling it either would be a lie — the
  # stripped Release binary has no symbol table at all.
  set(_symbol_scan_skip_code 77)

  # CMake knows the toolchain's nm; the bare name is the fallback for generators
  # that do not set it.
  set(_symbol_scan_nm "nm")
  if (CMAKE_NM)
    set(_symbol_scan_nm "${CMAKE_NM}")
  endif()

  # Namespaces and libraries rather than individual functions: what is excluded
  # is the whole of each, so a member added later is caught without this list
  # being touched. The overlay passes and the capture panel are inside
  # Assisi::Editor after moving there, so they need no entry of their own.
  set(_editor_symbols
    --forbid "Assisi::Editor::"
    --forbid "Assisi::Debug::"
    --forbid "ImGui::"
    --forbid "ImGui_Impl"
    --forbid "ImPlot"
    --forbid "ImGuizmo"
  )

  add_test(NAME GameLinksNoEditor
    COMMAND Python3::Interpreter "${_symbol_scanner}"
            --binary "$<TARGET_FILE:Assisi-Game>"
            --nm "${_symbol_scan_nm}"
            ${_editor_symbols}
  )
  set_tests_properties(GameLinksNoEditor PROPERTIES
    FIXTURES_REQUIRED GameBinary
    LABELS game
    SKIP_RETURN_CODE ${_symbol_scan_skip_code})

  # The positive control. GameEditor contains every one of those symbols by
  # design, so this scan must fail — and if it ever passes, the scanner has gone
  # blind (a demangler that stopped demangling, an nm whose output format moved)
  # and GameLinksNoEditor above is passing for the wrong reason.
  add_test(NAME GameEditorCarriesEditor
    COMMAND Python3::Interpreter "${_symbol_scanner}"
            --binary "$<TARGET_FILE:Assisi-GameEditor>"
            --nm "${_symbol_scan_nm}"
            ${_editor_symbols}
  )
  set_tests_properties(GameEditorCarriesEditor PROPERTIES
    WILL_FAIL TRUE
    SKIP_RETURN_CODE ${_symbol_scan_skip_code})

  # The game reads cooked content from its package and nothing else, so nothing
  # that reads or decodes source content belongs in it: not the sidecar index, not
  # a loose-file reader, not an importer, decoder or encoder, not the cook. One of
  # these appearing means a game that could quietly read files beside it, or
  # carries a pipeline it never runs.
  set(_source_pipeline_symbols
    --forbid "Assisi::Core::AssetDatabase"
    --forbid "Assisi::Core::LooseFileProvider"
    --forbid "Assisi::Core::ReadTextConfig"
    --forbid "Assisi::Runtime::SceneSerializer::ReadTextDocument"
    --forbid "Assisi::Editor::SourceAssetSource"
    --forbid "fastgltf::"
    --forbid "Assisi::Geometry::ImportMesh"
    --forbid "Assisi::Image::DecodeImage"
    --forbid "Assisi::Image::Compress("
    --forbid "Assisi::Cook::"
  )

  add_test(NAME GameLinksNoAssetDatabase
    COMMAND Python3::Interpreter "${_symbol_scanner}"
            --binary "$<TARGET_FILE:Assisi-Game>"
            --nm "${_symbol_scan_nm}"
            ${_source_pipeline_symbols}
  )
  set_tests_properties(GameLinksNoAssetDatabase PROPERTIES
    FIXTURES_REQUIRED GameBinary
    LABELS game
    SKIP_RETURN_CODE ${_symbol_scan_skip_code})

  # The positive control, as above: the editor reads source content, so it
  # carries these, and a scan that finds none of them has gone blind.
  add_test(NAME GameEditorCarriesSourcePipeline
    COMMAND Python3::Interpreter "${_symbol_scanner}"
            --binary "$<TARGET_FILE:Assisi-GameEditor>"
            --nm "${_symbol_scan_nm}"
            ${_source_pipeline_symbols}
  )
  set_tests_properties(GameEditorCarriesSourcePipeline PROPERTIES
    WILL_FAIL TRUE
    SKIP_RETURN_CODE ${_symbol_scan_skip_code})

  # The shared libraries the game asks a player's machine for: glibc and its
  # loader, which every Linux system has, and nothing else. The C++ runtime is
  # linked in, and the networking crypto is a static libsodium. The Vulkan loader
  # and the windowing libraries are not in the list because they are opened at
  # run time rather than linked, and they come with the graphics driver.
  #
  # libpthread and libdl are glibc too. Before 2.34 they are separate libraries
  # a threaded program links, and every later glibc still ships them, empty, so
  # a game built against an older one still starts.
  if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_dependency_checker "${CMAKE_SOURCE_DIR}/scripts/check-dependencies.py")
    set(_dependency_readelf "readelf")
    if (CMAKE_READELF)
      set(_dependency_readelf "${CMAKE_READELF}")
    endif()
    set(_system_libraries
      --allow libc.so.6
      --allow libm.so.6
      --allow libpthread.so.0
      --allow libdl.so.2
      --allow ld-linux-x86-64.so.2
    )

    # The ceiling goes on this check only. The control below is about the library
    # list, and a ceiling there would let it fail for the wrong reason.
    set(_glibc_ceiling "")
    if (ASSISI_GLIBC_CEILING)
      set(_glibc_ceiling --max-glibc "${ASSISI_GLIBC_CEILING}")
    endif()

    add_test(NAME GameNeedsOnlySystemLibraries
      COMMAND Python3::Interpreter "${_dependency_checker}"
              --binary "$<TARGET_FILE:Assisi-Game>"
              --readelf "${_dependency_readelf}"
              ${_system_libraries}
              ${_glibc_ceiling}
    )
    set_tests_properties(GameNeedsOnlySystemLibraries PROPERTIES
      FIXTURES_REQUIRED GameBinary
      LABELS game
      SKIP_RETURN_CODE ${_symbol_scan_skip_code})

    # The positive control: the same game against a list that leaves out libc,
    # which every dynamically linked Linux program needs, so the check must
    # refuse it. If it ever passes, the checker has stopped reading the dynamic
    # section and the test above passes for the wrong reason. The game rather than
    # the editor, because whether the editor links the C++ runtime shared depends
    # on the toolchain: the Steam Runtime's GCC links it statically by default.
    add_test(NAME GameDependencyCheckSeesLibc
      COMMAND Python3::Interpreter "${_dependency_checker}"
              --binary "$<TARGET_FILE:Assisi-Game>"
              --readelf "${_dependency_readelf}"
              --allow libm.so.6
              --allow libpthread.so.0
              --allow libdl.so.2
              --allow ld-linux-x86-64.so.2
    )
    set_tests_properties(GameDependencyCheckSeesLibc PROPERTIES
      FIXTURES_REQUIRED GameBinary
      LABELS game
      WILL_FAIL TRUE
      SKIP_RETURN_CODE ${_symbol_scan_skip_code})
  endif()

  # Registered beside the scans they underwrite rather than in a directory of
  # their own: the scanners exist for them, and a matcher that silently stopped
  # matching would make every one of them pass.
  add_test(NAME check-symbols
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/tests/test_check_symbols.py")
  add_test(NAME check-dependencies
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/tests/test_check_dependencies.py")

  # The Steam Runtime helper, with podman and docker faked: it starts no
  # container, so it runs on every machine.
  add_test(NAME steamrt
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/tests/test_steamrt.py")
endif()

# The source asset tree is the only one. Nothing is staged beside the executable,
# so a directory named "assets" there can only be a leftover from a build that
# predates that — and AssetSystem::DiscoverRoot walks upward from the executable,
# so a leftover is found first and shadows the real tree. Removing it at configure
# time is what stops such a build from silently reading assets nobody is editing.
file(REMOVE_RECURSE "${CMAKE_CURRENT_BINARY_DIR}/assets")

# --- Vulkan shaders: compile GLSL -> SPIR-V at build time.
#
# One target for both executables. The compiled .spv lands in the shared source
# asset tree, so two targets each owning a copy of these commands would be two
# rules writing one file — which ninja rejects outright when it notices, and
# which races when it does not.
#
# The copy into place is its own OUTPUT-tracked custom command per shader,
# depending on that shader's compiled .spv — NOT a POST_BUILD hook. A POST_BUILD
# hook only re-runs when the executable itself relinks, which a shader-only edit
# never triggers (no C++ source changed), so ninja would silently skip copying
# the freshly compiled SPIR-V and the app would keep running a stale shader.
#
# Auto-discovered — a new .vert/.frag/.comp under assets/shaders/ just needs the
# file created, no CMakeLists.txt edit. A .glsl file is a piece another shader
# #includes, never compiled on its own.
file(GLOB_RECURSE _vk_shader_sources
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${CMAKE_SOURCE_DIR}/assets/*.vert"
    "${CMAKE_SOURCE_DIR}/assets/*.frag"
    "${CMAKE_SOURCE_DIR}/assets/*.comp"
)

set(_shader_outputs "")

# Names this build directory, so concurrent builds of two presets write to
# different temporaries on their way into the shared asset tree.
get_filename_component(_assisi_build_id "${CMAKE_BINARY_DIR}" NAME)

# Compiles one GLSL source to SPIR-V, landing it beside its source in the asset
# tree.
#
# _shader_in   absolute path to the .vert/.frag/.comp
# _spv_rel     where the .spv lands, relative to assets/ — normally
#              "<source>.spv", and "<source>.<variant>.spv" for a variant built
#              from the same source with different defines
# ARGN         preprocessor defines for this build of the shader
#
# A shader keeps its path relative to assets/ (e.g. shaders/mesh.vert or
# editor/shaders/outline_edge.frag) through compilation, so editor-only shaders
# live under assets/editor/ and still resolve by their virtual path.
# Intermediate names in the binary dir flatten the slashes to stay unique and
# path-safe.
function(assisi_compile_shader _shader_in _spv_rel)
    set(_defines "")
    foreach(_define ${ARGN})
        list(APPEND _defines "-D${_define}")
    endforeach()

    get_filename_component(_spv_rel_dir "${_spv_rel}" DIRECTORY)
    string(REPLACE "/" "_" _shader_flat "${_spv_rel}")
    set(_shader_out "${CMAKE_CURRENT_BINARY_DIR}/${_shader_flat}")
    # The depfile names every file the source #includes, so editing one of the
    # .glsl pieces a shader is assembled from rebuilds it; DEPENDS alone would
    # see only the top-level source.
    set(_shader_dep "${_shader_out}.d")
    add_custom_command(
        OUTPUT "${_shader_out}"
        COMMAND $<TARGET_FILE:glslang-standalone> --target-env vulkan1.3 ${_defines} --depfile "${_shader_dep}"
                "${_shader_in}" -o "${_shader_out}"
        DEPENDS "${_shader_in}" glslang-standalone
        DEPFILE "${_shader_dep}"
        COMMENT "Compiling ${_spv_rel}"
        VERBATIM
    )

    # Compiled .spv lands beside its GLSL source in the asset tree, so it resolves
    # through Core::AssetSystem exactly like every other asset. The compile above
    # writes the binary dir instead of landing here directly because glslang's
    # depfile names its -o path as the target, and ninja rejects a depfile whose
    # target is not the command's own OUTPUT.
    #
    # Every preset shares this destination, so the write goes to a temporary
    # named for this build directory and is then renamed into place: rename is
    # atomic within a filesystem, and the temporary sits in the destination
    # directory to guarantee it is the same one. Two presets building the same
    # shader at once therefore cannot leave a half-written .spv for the app to
    # load.
    set(_shader_final "${CMAKE_SOURCE_DIR}/assets/${_spv_rel}")
    set(_shader_tmp "${_shader_final}.${_assisi_build_id}.tmp")
    add_custom_command(
        OUTPUT "${_shader_final}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/assets/${_spv_rel_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy "${_shader_out}" "${_shader_tmp}"
        COMMAND ${CMAKE_COMMAND} -E rename "${_shader_tmp}" "${_shader_final}"
        DEPENDS "${_shader_out}"
        COMMENT "Staging ${_spv_rel}"
        VERBATIM
    )
    set(_outputs ${_shader_outputs})
    list(APPEND _outputs "${_shader_final}")
    set(_shader_outputs "${_outputs}" PARENT_SCOPE)
endfunction()

foreach(_shader_in ${_vk_shader_sources})
    file(RELATIVE_PATH _shader_rel "${CMAKE_SOURCE_DIR}/assets" "${_shader_in}")
    assisi_compile_shader("${_shader_in}" "${_shader_rel}.spv")
endforeach()

# Shader variants: the same source compiled again with a define, landing beside
# the base .spv as <source>.<variant>.spv. The mesh pass needs mesh.frag both
# with and without its alpha-test discard, because a shader that can discard
# costs its whole pipeline early depth rejection — so the opaque geometry draws
# through the build that cannot.
assisi_compile_shader("${CMAKE_SOURCE_DIR}/assets/shaders/mesh.frag" "shaders/mesh.frag.masked.spv"
    ASSISI_ALPHA_MASK)

# The shadow depth stage the same way round: the base build reads position only
# and has no fragment stage, and this one carries the UV and material row that
# shadow_depth.frag alpha-tests against.
assisi_compile_shader("${CMAKE_SOURCE_DIR}/assets/shaders/shadow_depth.vert"
    "shaders/shadow_depth.vert.masked.spv" ASSISI_ALPHA_MASK)

# The depth prepass screen-space occlusion reads, and the lit pass that draws
# over it. The plain mesh.vert build stays the lit pass whenever there is no
# prepass, so a frame with occlusion off draws through exactly the stage it
# always did; these three declare their position invariant so the lit pass's
# depth matches the prepass's to the bit.
assisi_compile_shader("${CMAKE_SOURCE_DIR}/assets/shaders/mesh.vert"
    "shaders/mesh.vert.depth.spv" ASSISI_DEPTH_ONLY)
assisi_compile_shader("${CMAKE_SOURCE_DIR}/assets/shaders/mesh.vert"
    "shaders/mesh.vert.depth.masked.spv" ASSISI_DEPTH_ONLY ASSISI_ALPHA_MASK)
assisi_compile_shader("${CMAKE_SOURCE_DIR}/assets/shaders/mesh.vert"
    "shaders/mesh.vert.invariant.spv" ASSISI_INVARIANT_POSITION)

# The scene-distance step every screen-space feature reads, for a multisampled
# depth buffer.
assisi_compile_shader("${CMAKE_SOURCE_DIR}/assets/shaders/scene_distance.frag"
    "shaders/scene_distance.frag.msaa.spv" ASSISI_MSAA_DEPTH)

add_custom_target(Assisi-VkShaders ALL DEPENDS ${_shader_outputs})
add_dependencies(Assisi-Game Assisi-VkShaders)
add_dependencies(Assisi-GameEditor Assisi-VkShaders)
