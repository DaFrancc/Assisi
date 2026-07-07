add_executable(Assisi-VkTriangle)
Assisi_apply_defaults(Assisi-VkTriangle)

target_sources(Assisi-VkTriangle
  PRIVATE
    "src/main.cpp"
)

target_link_libraries(Assisi-VkTriangle
  PRIVATE
    Assisi::Window
    Assisi::Core
    nvrhi
    nvrhi_vk
    Vulkan::Headers
    glfw
)

# --- Compile GLSL shaders to SPIR-V at build time via glslang's standalone compiler.
set(_vk_triangle_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(_vk_triangle_spv_outputs "")

foreach(_shader_name triangle.vert triangle.frag)
    set(_shader_in "${_vk_triangle_shader_dir}/${_shader_name}")
    set(_shader_out "${CMAKE_CURRENT_BINARY_DIR}/${_shader_name}.spv")
    add_custom_command(
        OUTPUT "${_shader_out}"
        COMMAND $<TARGET_FILE:glslang-standalone> -V "${_shader_in}" -o "${_shader_out}"
        DEPENDS "${_shader_in}" glslang-standalone
        COMMENT "Compiling ${_shader_name} to SPIR-V"
        VERBATIM
    )
    list(APPEND _vk_triangle_spv_outputs "${_shader_out}")
endforeach()

add_custom_target(Assisi-VkTriangle-Shaders ALL DEPENDS ${_vk_triangle_spv_outputs})
add_dependencies(Assisi-VkTriangle Assisi-VkTriangle-Shaders)

add_custom_command(TARGET Assisi-VkTriangle POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Assisi-VkTriangle>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_vk_triangle_spv_outputs} "$<TARGET_FILE_DIR:Assisi-VkTriangle>/shaders"
    COMMENT "Copying compiled shaders next to Assisi-VkTriangle"
)
