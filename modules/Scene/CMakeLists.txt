add_library(Assisi-Scene)

target_sources(Assisi-Scene
  PRIVATE
    src/Camera.cpp
)

target_include_directories(Assisi-Scene
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(Assisi-Scene
  PUBLIC
    Assisi::Core
    Assisi::Math
    Assisi::Render
)

add_library(Assisi::Scene ALIAS Assisi-Scene)
