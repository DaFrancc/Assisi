# The project's own code, shared by both executables: the game's systems, the
# headless server, and the argument parsers. A static library rather than a set
# of sources listed twice, so the two binaries cannot drift in what they contain.
#
# The reflection registrations deliberately do NOT live here — see the INTERFACE
# library below for why an archive is the wrong place for them.
add_library(Assisi-GameLib STATIC)
Assisi_apply_defaults(Assisi-GameLib)

target_sources(Assisi-GameLib
  PRIVATE
    "src/DemoSystems.cpp"
    "src/DemoSystems.hpp"
    "src/LaunchArgs.cpp"
    "src/LaunchArgs.hpp"
    "src/ServerApp.cpp"
    "src/ServerApp.hpp"
)

target_include_directories(Assisi-GameLib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

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

# The game's own ASYSTEM declarations. Scanning them is what puts them in the
# catalog: a level names a system, and the declaration in a reflected header is
# the registration — there is no registerGameSystems to call.
#
# Reflected on an INTERFACE lib rather than on Assisi-GameLib, and the generated
# objects are added to each executable rather than to the archive. Both halves
# matter: assisi_reflect links its generated OBJECT library into its target, and
# a static library would let the linker drop those objects as unreferenced —
# which silently discards every registration in them.
add_library(Assisi-Game-Systems INTERFACE)
target_include_directories(Assisi-Game-Systems INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Assisi-Game-Systems INTERFACE Assisi::App)
assisi_reflect(
  TARGET Assisi-Game-Systems
  HEADERS
    src/DemoSystems.hpp
)

# --- The two executables.
#
# Game links no editor and has no way to reach one; GameEditor is the same game
# with Assisi::Editor on top. The editor is a library rather than its own
# executable because reflection registers per final binary — a separate editor
# process could not inspect the components this project's code declares.
add_executable(Assisi-Game)
Assisi_apply_defaults(Assisi-Game)
target_sources(Assisi-Game PRIVATE "src/GameMain.cpp")
target_link_libraries(Assisi-Game PRIVATE Assisi-GameLib Assisi-Game-Systems)

add_executable(Assisi-GameEditor)
Assisi_apply_defaults(Assisi-GameEditor)
target_sources(Assisi-GameEditor PRIVATE "src/EditorMain.cpp")
target_link_libraries(Assisi-GameEditor PRIVATE Assisi-GameLib Assisi-Game-Systems Assisi::Editor)

# Per-binary, for the reason stated above the INTERFACE library: an executable
# cannot be linked into, so its registrations arrive as objects.
foreach(_exe Assisi-Game Assisi-GameEditor)
  target_sources("${_exe}" PRIVATE $<TARGET_OBJECTS:Assisi-Game-Systems-Generated>)
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
  add_test(NAME GameBoots
    COMMAND Assisi-Game --headless --ticks 1
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:Assisi-Game>"
  )
  # A game that never reaches its tick limit would otherwise hang the suite
  # rather than fail it.
  set_tests_properties(GameBoots PROPERTIES TIMEOUT 60)
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

  # The game still builds its GUID->path index by scanning sidecars at startup,
  # so it links the mutable database and the loose-file provider and cannot yet
  # do without them. WILL_FAIL records that as a known state rather than a
  # forgotten one: when a baked pak index replaces the scan, this test starts
  # passing, WILL_FAIL turns that into a failure, and whoever landed the index
  # removes the property. The alternative is a comment nothing checks.
  add_test(NAME GameLinksNoAssetDatabase
    COMMAND Python3::Interpreter "${_symbol_scanner}"
            --binary "$<TARGET_FILE:Assisi-Game>"
            --nm "${_symbol_scan_nm}"
            --forbid "Assisi::Core::AssetDatabase"
            --forbid "Assisi::Core::LooseFileProvider"
  )
  set_tests_properties(GameLinksNoAssetDatabase PROPERTIES
    WILL_FAIL TRUE
    SKIP_RETURN_CODE ${_symbol_scan_skip_code})

  # Registered beside the scans it underwrites rather than in a directory of its
  # own: the scanner exists for them, and a matcher that silently stopped
  # matching would make all three pass.
  add_test(NAME check-symbols
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/tests/test_check_symbols.py")
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
