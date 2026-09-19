# The engine side of Mondrian: what puts the core's draw list on screen. It is
# the only part that may name the renderer.
add_library(Assisi-MondrianEngine)
Assisi_apply_defaults(Assisi-MondrianEngine)

target_sources(Assisi-MondrianEngine
  PUBLIC
    "include/Assisi/Mondrian/Engine/QuadPass.hpp"
  PRIVATE
    "src/QuadPass.cpp"
)

target_include_directories(Assisi-MondrianEngine
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-MondrianEngine
  PUBLIC
    Assisi::Mondrian
    Assisi::Render
)

add_library(Assisi::MondrianEngine ALIAS Assisi-MondrianEngine)
