# Mondrian, the game UI, in two layers. The core lays out, handles focus and
# builds a draw list from pushed input; it knows nothing of the GPU, the window
# or the world, which is what lets every part of it be tested headless. The
# engine layer in engine/ is what puts that draw list on screen.
add_library(Assisi-Mondrian)
Assisi_apply_defaults(Assisi-Mondrian)

target_sources(Assisi-Mondrian
  PUBLIC
    "include/Assisi/Mondrian/DrawList.hpp"
    "include/Assisi/Mondrian/Ui.hpp"
  PRIVATE
    "src/Ui.cpp"
)

target_include_directories(Assisi-Mondrian
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Mondrian
  PUBLIC
    Assisi::Core
)

add_library(Assisi::Mondrian ALIAS Assisi-Mondrian)

# The boundary above, held by the configure rather than by review: an edge from
# the core to any of these fails before anything compiles.
include("${CMAKE_SOURCE_DIR}/cmake/AssisiLinkGuard.cmake")
assisi_assert_link_excludes(Assisi-Mondrian
    Assisi-Render
    Assisi::Render
    Assisi-Window
    Assisi::Window
    Assisi-ECS
    Assisi::ECS
)

add_subdirectory(engine)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
