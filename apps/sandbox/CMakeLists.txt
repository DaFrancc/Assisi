add_executable(Assisi-Sandbox)
Assisi_apply_defaults(Assisi-Sandbox)

# A thin consumer of the editor library: main.cpp builds an EditorConfig and
# runs Assisi::Editor::EditorApp. The exe target still owns what must be
# per-binary: reflection linking (below) and asset staging.
target_sources(Assisi-Sandbox
  PRIVATE
    "src/DemoSystems.cpp"
    "src/DemoSystems.hpp"
    "src/main.cpp"
    "src/ServerApp.cpp"
    "src/ServerApp.hpp"
)

target_link_libraries(Assisi-Sandbox
  PRIVATE
    Assisi::Editor
    # ServerApp derives from Application directly and never touches the editor;
    # named explicitly rather than leaned on through Assisi::Editor's transitive
    # link, so the dependency survives the editor being dropped from a build.
    Assisi::App
    Assisi::Physics
)

# ServerApp keeps its Offline role without networking — headless simulation is a
# separate thing from replication — so the file stays in the build and guards the
# Host/Client halves internally.
if (ASSISI_ENABLE_NETWORKING)
  target_link_libraries(Assisi-Sandbox PRIVATE Assisi::NetSync)
endif()

# A refused start has to reach the exit code (round-7 B18). Driven as a real
# process because that is the whole claim — `StartupFailed()` returning true is
# not the property anyone depends on, `$?` being non-zero is, and only main()
# joins the two.
#
# `--connect` at a name the transport will not resolve is the cheapest of the
# five refusal paths to reach: NetTransport::Connect takes IP literals only, so
# this fails inside NetSession::Join, synchronously, during OnStart — no peer to
# stand up, no port to bind, no timeout to wait out, and nothing about it varies
# with the machine. WILL_FAIL asserts the non-zero exit.
if (ASSISI_BUILD_TESTS AND ASSISI_ENABLE_NETWORKING)
  add_test(NAME SandboxRefusedStart
    COMMAND Assisi-Sandbox --connect not-an-ip:27015 --ticks 1
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:Assisi-Sandbox>"
  )
  set_tests_properties(SandboxRefusedStart PROPERTIES WILL_FAIL TRUE)
endif()

# The game's own ASYSTEM declarations. Scanning them is what puts them in the
# catalog: a level names a system, and the declaration in a reflected header is
# the registration — there is no registerGameSystems to call.
#
# Reflected on an INTERFACE lib rather than on the executable, because
# assisi_reflect links the generated OBJECT library into its target and an
# executable cannot be linked into. Same shape the NetSync tests use.
add_library(Assisi-Sandbox-Systems INTERFACE)
target_include_directories(Assisi-Sandbox-Systems INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Assisi-Sandbox-Systems INTERFACE Assisi::App)
assisi_reflect(
  TARGET Assisi-Sandbox-Systems
  HEADERS
    src/DemoSystems.hpp
)
target_link_libraries(Assisi-Sandbox PRIVATE Assisi-Sandbox-Systems)
target_sources(Assisi-Sandbox PRIVATE $<TARGET_OBJECTS:Assisi-Sandbox-Systems-Generated>)

assisi_link_reflections(Assisi-Sandbox)

# The source asset tree is the only one. Nothing is staged beside the executable,
# so a directory named "assets" there can only be a leftover from a build that
# predates that — and AssetSystem::DiscoverRoot walks upward from the executable,
# so a leftover is found first and shadows the real tree. Removing it at configure
# time is what stops such a build from silently reading assets nobody is editing.
file(REMOVE_RECURSE "${CMAKE_CURRENT_BINARY_DIR}/assets")

# --- Vulkan shaders: compile GLSL -> SPIR-V at build time.
#
# The copy into place is its own OUTPUT-tracked custom command per shader,
# depending on that shader's compiled .spv — NOT a POST_BUILD hook on
# Assisi-Sandbox. A POST_BUILD hook only re-runs when the executable itself
# relinks, which a shader-only edit never triggers (no C++ source changed),
# so ninja would silently skip copying the freshly compiled SPIR-V next to
# the exe and the app would keep running a stale shader.
#
# Auto-discovered — a new .vert/.frag/.comp under assets/shaders/ just needs the
# file created, no CMakeLists.txt edit. A .glsl file is a piece another shader
# #includes, never compiled on its own.
file(GLOB_RECURSE _sandbox_vk_shader_sources
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${CMAKE_SOURCE_DIR}/assets/*.vert"
    "${CMAKE_SOURCE_DIR}/assets/*.frag"
    "${CMAKE_SOURCE_DIR}/assets/*.comp"
)

set(_sandbox_shader_outputs "")

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
function(assisi_sandbox_compile_shader _shader_in _spv_rel)
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
    set(_outputs ${_sandbox_shader_outputs})
    list(APPEND _outputs "${_shader_final}")
    set(_sandbox_shader_outputs "${_outputs}" PARENT_SCOPE)
endfunction()

foreach(_shader_in ${_sandbox_vk_shader_sources})
    file(RELATIVE_PATH _shader_rel "${CMAKE_SOURCE_DIR}/assets" "${_shader_in}")
    assisi_sandbox_compile_shader("${_shader_in}" "${_shader_rel}.spv")
endforeach()

# Shader variants: the same source compiled again with a define, landing beside
# the base .spv as <source>.<variant>.spv. The mesh pass needs mesh.frag both
# with and without its alpha-test discard, because a shader that can discard
# costs its whole pipeline early depth rejection — so the opaque geometry draws
# through the build that cannot.
assisi_sandbox_compile_shader("${CMAKE_SOURCE_DIR}/assets/shaders/mesh.frag" "shaders/mesh.frag.masked.spv"
    ASSISI_ALPHA_MASK)

# The shadow depth stage the same way round: the base build reads position only
# and has no fragment stage, and this one carries the UV and material row that
# shadow_depth.frag alpha-tests against.
assisi_sandbox_compile_shader("${CMAKE_SOURCE_DIR}/assets/shaders/shadow_depth.vert"
    "shaders/shadow_depth.vert.masked.spv" ASSISI_ALPHA_MASK)

# The depth prepass screen-space occlusion reads, and the lit pass that draws
# over it. The plain mesh.vert build stays the lit pass whenever there is no
# prepass, so a frame with occlusion off draws through exactly the stage it
# always did; these three declare their position invariant so the lit pass's
# depth matches the prepass's to the bit.
assisi_sandbox_compile_shader("${CMAKE_SOURCE_DIR}/assets/shaders/mesh.vert"
    "shaders/mesh.vert.depth.spv" ASSISI_DEPTH_ONLY)
assisi_sandbox_compile_shader("${CMAKE_SOURCE_DIR}/assets/shaders/mesh.vert"
    "shaders/mesh.vert.depth.masked.spv" ASSISI_DEPTH_ONLY ASSISI_ALPHA_MASK)
assisi_sandbox_compile_shader("${CMAKE_SOURCE_DIR}/assets/shaders/mesh.vert"
    "shaders/mesh.vert.invariant.spv" ASSISI_INVARIANT_POSITION)

# The scene-distance step every screen-space feature reads, for a multisampled
# depth buffer.
assisi_sandbox_compile_shader("${CMAKE_SOURCE_DIR}/assets/shaders/scene_distance.frag"
    "shaders/scene_distance.frag.msaa.spv" ASSISI_MSAA_DEPTH)

add_custom_target(Assisi-Sandbox-VkShaders ALL DEPENDS ${_sandbox_shader_outputs})
add_dependencies(Assisi-Sandbox Assisi-Sandbox-VkShaders)
