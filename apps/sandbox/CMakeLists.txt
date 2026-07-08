add_executable(Assisi-Sandbox)
Assisi_apply_defaults(Assisi-Sandbox)

target_sources(Assisi-Sandbox
  PRIVATE
    "src/main.cpp"
)

target_link_libraries(Assisi-Sandbox
  PRIVATE
    Assisi::App
    nlohmann_json::nlohmann_json
)

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
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:Assisi-Sandbox>/assets"
    COMMAND ${CMAKE_COMMAND} -E touch "${_assets_stamp}"
    DEPENDS ${_assisi_assets}
    COMMENT "Copying assets to build directory"
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
    "${CMAKE_SOURCE_DIR}/assets/shaders/*.vert"
    "${CMAKE_SOURCE_DIR}/assets/shaders/*.frag"
    "${CMAKE_SOURCE_DIR}/assets/shaders/*.comp"
)

set(_sandbox_shader_copies "")

foreach(_shader_in ${_sandbox_vk_shader_sources})
    get_filename_component(_shader_name "${_shader_in}" NAME)
    set(_shader_out "${CMAKE_CURRENT_BINARY_DIR}/${_shader_name}.spv")
    add_custom_command(
        OUTPUT "${_shader_out}"
        COMMAND $<TARGET_FILE:glslang-standalone> -V "${_shader_in}" -o "${_shader_out}"
        DEPENDS "${_shader_in}" glslang-standalone
        COMMENT "Compiling ${_shader_name} to SPIR-V"
        VERBATIM
    )

    # OUTPUT must be a literal path (no generator expressions) for ninja's
    # dependency scanner, so track completion via a stamp file in the binary
    # dir; the actual copy destination can still use TARGET_FILE_DIR since
    # that's fine inside COMMAND.
    # Compiled .spv lands under the copied asset root (assets/shaders/), NOT a
    # sibling shaders/ dir, so it resolves through Core::AssetSystem exactly like
    # every other asset. The GLSL source is already mirrored there by the asset
    # copy above; the compiled output sits alongside it.
    set(_shader_stamp "${CMAKE_CURRENT_BINARY_DIR}/${_shader_name}.spv.copied")
    add_custom_command(
        OUTPUT "${_shader_stamp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Assisi-Sandbox>/assets/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_shader_out}" "$<TARGET_FILE_DIR:Assisi-Sandbox>/assets/shaders/${_shader_name}.spv"
        COMMAND ${CMAKE_COMMAND} -E touch "${_shader_stamp}"
        DEPENDS "${_shader_out}"
        COMMENT "Copying ${_shader_name}.spv into the Assisi-Sandbox asset root"
        VERBATIM
    )
    list(APPEND _sandbox_shader_copies "${_shader_stamp}")
endforeach()

add_custom_target(Assisi-Sandbox-VkShaders ALL DEPENDS ${_sandbox_shader_copies})
add_dependencies(Assisi-Sandbox Assisi-Sandbox-VkShaders)