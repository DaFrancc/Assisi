# Mondrian, the game UI, in two layers. The core lays out, handles focus and
# builds a draw list from pushed input; it knows nothing of the GPU, the window
# or the world, which is what lets every part of it be tested headless. The
# engine layer in engine/ is what puts that draw list on screen.
add_library(Assisi-Mondrian)
Assisi_apply_defaults(Assisi-Mondrian)

target_sources(Assisi-Mondrian
  PUBLIC
    "include/Assisi/Mondrian/Clipboard.hpp"
    "include/Assisi/Mondrian/Draw.hpp"
    "include/Assisi/Mondrian/DrawList.hpp"
    "include/Assisi/Mondrian/Font.hpp"
    "include/Assisi/Mondrian/FontDescription.hpp"
    "include/Assisi/Mondrian/FontReader.hpp"
    "include/Assisi/Mondrian/HitTest.hpp"
    "include/Assisi/Mondrian/Input.hpp"
    "include/Assisi/Mondrian/Layout.hpp"
    "include/Assisi/Mondrian/Navigation.hpp"
    "include/Assisi/Mondrian/NodeTree.hpp"
    "include/Assisi/Mondrian/Style.hpp"
    "include/Assisi/Mondrian/Text.hpp"
    "include/Assisi/Mondrian/Ui.hpp"
    "include/Assisi/Mondrian/Utf8.hpp"
  PRIVATE
    "src/Draw.cpp"
    "src/DrawList.cpp"
    "src/Font.cpp"
    "src/FontReader.cpp"
    "src/HitTest.cpp"
    "src/Layout.cpp"
    "src/Navigation.cpp"
    "src/NodeTree.cpp"
    "src/Text.cpp"
    "src/Ui.cpp"
    "src/Utf8.cpp"
)

target_include_directories(Assisi-Mondrian
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Mondrian
  PUBLIC
    Assisi::Core
    Assisi::Math
)

add_library(Assisi::Mondrian ALIAS Assisi-Mondrian)

assisi_reflect(
  TARGET Assisi-Mondrian
  HEADERS
    include/Assisi/Mondrian/FontDescription.hpp
)

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
add_subdirectory(import)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
