# Font import: FreeType and the rasteriser that turns a font description into a
# cooked Font. The cook and the editor link it; the game must not, and its link
# guard says so.
add_library(Assisi-FontImport)
Assisi_apply_defaults(Assisi-FontImport)

target_sources(Assisi-FontImport
  PUBLIC
    "include/Assisi/Mondrian/Import/FontImport.hpp"
  PRIVATE
    "src/FontImport.cpp"
)

target_include_directories(Assisi-FontImport
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-FontImport
  PUBLIC
    Assisi::Mondrian
  PRIVATE
    Freetype::Freetype
)

add_library(Assisi::FontImport ALIAS Assisi-FontImport)

# Screen import: the markup parser and the compiler that turns a parsed file
# into a ScreenDocument. The cook and the editor link it; the game must not, and
# its link guard says so.
#
# Separate from the cooker in modules/Cook on purpose. The editor compiles a
# screen in process and does not link Assisi::Cook, so the text-to-bytes half
# has to live where both can reach it, with the cooker as a caller.
add_library(Assisi-MondrianImport)
Assisi_apply_defaults(Assisi-MondrianImport)

target_sources(Assisi-MondrianImport
  PUBLIC
    "include/Assisi/Mondrian/Import/Markup.hpp"
    "include/Assisi/Mondrian/Import/ScreenCompiler.hpp"
  PRIVATE
    "src/Markup.cpp"
    "src/MarkupValues.cpp"
    "src/MarkupValues.hpp"
    "src/ScreenCompiler.cpp"
)

target_include_directories(Assisi-MondrianImport
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-MondrianImport
  PUBLIC
    Assisi::Mondrian
)

add_library(Assisi::MondrianImport ALIAS Assisi-MondrianImport)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
