add_library(Assisi-Game)

target_sources(Assisi-Game
  PRIVATE
    src/Camera.cpp
 "src/SpawnSystem.cpp")

target_include_directories(Assisi-Game
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(Assisi-Game
  PUBLIC
    Assisi::Core
    Assisi::Math
    Assisi::Render
)

add_library(Assisi::Game ALIAS Assisi-Game)
