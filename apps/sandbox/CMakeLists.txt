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
# with the machine. WILL_FAIL is the assertion: the test passes only on a
# non-zero exit, which is exactly what B18 says the server does not produce.
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

# Glob all asset files so the copy re-runs whenever any file changes.
# CONFIGURE_DEPENDS also triggers a re-configure when files are added or removed.
file(GLOB_RECURSE _assisi_assets
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${CMAKE_SOURCE_DIR}/assets/*"
)

set(_assets_stamp "${CMAKE_CURRENT_BINARY_DIR}/assets.stamp")

add_custom_command(
    OUTPUT "${_assets_stamp}"
    # copy_directory only adds and overwrites, so prune first: a deleted or
    # renamed source asset would otherwise linger in the staged tree forever and
    # still resolve through AssetSystem. Compiled .spv files are exempt (they are
    # build outputs with no source counterpart) — see the script for why this is
    # a targeted prune rather than a wipe-and-recopy.
    COMMAND ${CMAKE_COMMAND}
        -DSRC_DIR=${CMAKE_SOURCE_DIR}/assets
        -DDST_DIR=$<TARGET_FILE_DIR:Assisi-Sandbox>/assets
        -P "${CMAKE_SOURCE_DIR}/cmake/AssisiPruneStagedAssets.cmake"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:Assisi-Sandbox>/assets"
    COMMAND ${CMAKE_COMMAND} -E touch "${_assets_stamp}"
    DEPENDS ${_assisi_assets} "${CMAKE_SOURCE_DIR}/cmake/AssisiPruneStagedAssets.cmake"
    COMMENT "Staging assets to the build directory (pruning stale files first)"
    VERBATIM
)

add_custom_target(Assisi-CopyAssets ALL
    DEPENDS "${_assets_stamp}"
)

add_dependencies(Assisi-Sandbox Assisi-CopyAssets)

# --- Vulkan shaders: compile GLSL -> SPIR-V at build time (OpenGL shaders are
# compiled at runtime instead; these Vulkan-only shaders are the exception
# during the migration, matching apps/vk_triangle's approach).
#
# The copy into place is its own OUTPUT-tracked custom command per shader,
# depending on that shader's compiled .spv — NOT a POST_BUILD hook on
# Assisi-Sandbox. A POST_BUILD hook only re-runs when the executable itself
# relinks, which a shader-only edit never triggers (no C++ source changed),
# so ninja would silently skip copying the freshly compiled SPIR-V next to
# the exe and the app would keep running a stale shader. Learned this the
# hard way debugging what looked like a persistent lighting bug that was
# actually just an out-of-date .spv.
# Auto-discovered, like the assets glob above — a new .vert/.frag/.comp under
# assets/shaders/ just needs the file created, no CMakeLists.txt edit. (The old
# hardcoded file list was easy to forget to update: add a shader, wire it up in
# C++, and get a runtime "file not found" instead of a build error.)
file(GLOB_RECURSE _sandbox_vk_shader_sources
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${CMAKE_SOURCE_DIR}/assets/*.vert"
    "${CMAKE_SOURCE_DIR}/assets/*.frag"
    "${CMAKE_SOURCE_DIR}/assets/*.comp"
)

set(_sandbox_shader_copies "")

foreach(_shader_in ${_sandbox_vk_shader_sources})
    # A shader keeps its path relative to assets/ (e.g. shaders/cube_min.vert or
    # editor/shaders/outline_edge.frag) through compilation and into the copied
    # asset root, so editor-only shaders live under assets/editor/ and still
    # resolve by their virtual path. Intermediate names in the binary dir flatten
    # the slashes to stay unique and path-safe.
    file(RELATIVE_PATH _shader_rel "${CMAKE_SOURCE_DIR}/assets" "${_shader_in}")
    get_filename_component(_shader_rel_dir "${_shader_rel}" DIRECTORY)
    string(REPLACE "/" "_" _shader_flat "${_shader_rel}")
    set(_shader_out "${CMAKE_CURRENT_BINARY_DIR}/${_shader_flat}.spv")
    add_custom_command(
        OUTPUT "${_shader_out}"
        COMMAND $<TARGET_FILE:glslang-standalone> --target-env vulkan1.3 "${_shader_in}" -o "${_shader_out}"
        DEPENDS "${_shader_in}" glslang-standalone
        COMMENT "Compiling ${_shader_rel} to SPIR-V"
        VERBATIM
    )

    # OUTPUT must be a literal path (no generator expressions) for ninja's
    # dependency scanner, so track completion via a stamp file in the binary
    # dir; the actual copy destination can still use TARGET_FILE_DIR since
    # that's fine inside COMMAND.
    # Compiled .spv lands beside its GLSL source under the copied asset root, so it
    # resolves through Core::AssetSystem exactly like every other asset.
    set(_shader_stamp "${CMAKE_CURRENT_BINARY_DIR}/${_shader_flat}.spv.copied")
    add_custom_command(
        OUTPUT "${_shader_stamp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Assisi-Sandbox>/assets/${_shader_rel_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_shader_out}" "$<TARGET_FILE_DIR:Assisi-Sandbox>/assets/${_shader_rel}.spv"
        COMMAND ${CMAKE_COMMAND} -E touch "${_shader_stamp}"
        DEPENDS "${_shader_out}"
        COMMENT "Copying ${_shader_rel}.spv into the Assisi-Sandbox asset root"
        VERBATIM
    )
    list(APPEND _sandbox_shader_copies "${_shader_stamp}")
endforeach()

add_custom_target(Assisi-Sandbox-VkShaders ALL DEPENDS ${_sandbox_shader_copies})
add_dependencies(Assisi-Sandbox Assisi-Sandbox-VkShaders)
