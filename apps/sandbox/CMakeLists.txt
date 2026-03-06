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
)

add_custom_command(TARGET Assisi-Sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:Assisi-Sandbox>/assets"
    COMMENT "Copying assets next to Assisi-Sandbox"
)