add_executable(Assisi-Sandbox)
Assisi_apply_defaults(Assisi-Sandbox)

target_sources(Assisi-Sandbox
  PRIVATE
    "src/main.cpp"
)

find_package(OpenGL REQUIRED)

target_link_libraries(Assisi-Sandbox
  PRIVATE
    Assisi::Render
    Assisi::ECS
    Assisi::Game
    Assisi::Window
    Assisi::Deps
    OpenGL::GL
)
