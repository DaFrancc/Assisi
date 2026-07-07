add_executable(Assisi-Sandbox)
Assisi_apply_defaults(Assisi-Sandbox)

target_sources(Assisi-Sandbox
  PRIVATE
    "src/main.cpp"
)

find_package(OpenGL REQUIRED)

target_link_libraries(Assisi-Sandbox
  PRIVATE
    Assisi::App
    OpenGL::GL
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
set(_sandbox_vk_shader_dir "${CMAKE_SOURCE_DIR}/assets/shaders")
set(_sandbox_vk_spv_outputs "")

foreach(_shader_name cube_min.vert cube_min.frag imgui_opener.vert imgui_opener.frag
                     cluster_build.comp cluster_cull.comp)
    set(_shader_in "${_sandbox_vk_shader_dir}/${_shader_name}")
    set(_shader_out "${CMAKE_CURRENT_BINARY_DIR}/${_shader_name}.spv")
    add_custom_command(
        OUTPUT "${_shader_out}"
        COMMAND $<TARGET_FILE:glslang-standalone> -V "${_shader_in}" -o "${_shader_out}"
        DEPENDS "${_shader_in}" glslang-standalone
        COMMENT "Compiling ${_shader_name} to SPIR-V"
        VERBATIM
    )
    list(APPEND _sandbox_vk_spv_outputs "${_shader_out}")
endforeach()

add_custom_target(Assisi-Sandbox-VkShaders ALL DEPENDS ${_sandbox_vk_spv_outputs})
add_dependencies(Assisi-Sandbox Assisi-Sandbox-VkShaders)

add_custom_command(TARGET Assisi-Sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Assisi-Sandbox>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_sandbox_vk_spv_outputs} "$<TARGET_FILE_DIR:Assisi-Sandbox>/shaders"
    COMMENT "Copying compiled Vulkan shaders next to Assisi-Sandbox"
)