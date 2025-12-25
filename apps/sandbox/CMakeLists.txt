add_executable(Assisi-Sandbox src/main.cpp)

target_sources(Assisi-Sandbox
  PRIVATE
    src/main.cpp
)

find_package(OpenGL REQUIRED)

target_link_libraries(Assisi-Sandbox
  PRIVATE
    Assisi::Render
    Assisi::ECS
    Assisi::Game
    Assisi::Window
    Assisi::Deps
    Assisi::glad
    OpenGL::GL
)
