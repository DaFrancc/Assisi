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

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
