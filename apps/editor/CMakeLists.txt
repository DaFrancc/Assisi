if(ASSISI_EDITOR)
    set(_app_target Assisi-Editor)
else()
    set(_app_target Assisi-Game)
endif()

add_executable(${_app_target})
Assisi_apply_defaults(${_app_target})

target_sources(${_app_target}
    PRIVATE
        "src/main.cpp"
)

target_link_libraries(${_app_target}
    PRIVATE
        Assisi::App
        nlohmann_json::nlohmann_json
)

if(ASSISI_EDITOR)
    target_compile_definitions(${_app_target}
        PRIVATE
            ASSISI_EDITOR=1
            # Bake the project root so the editor can invoke cmake for game export.
            ASSISI_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
    )
    target_link_libraries(${_app_target} PRIVATE Assisi::Debug)
    assisi_link_reflections(${_app_target})
endif()

# Copy assets next to the executable for both editor and game builds.
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
        "$<TARGET_FILE_DIR:${_app_target}>/assets"
    COMMAND ${CMAKE_COMMAND} -E touch "${_assets_stamp}"
    DEPENDS ${_assisi_assets}
    COMMENT "Copying assets to build directory"
    VERBATIM
)

add_custom_target(${_app_target}-CopyAssets ALL
    DEPENDS "${_assets_stamp}"
)

add_dependencies(${_app_target} ${_app_target}-CopyAssets)
