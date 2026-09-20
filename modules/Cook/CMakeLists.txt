add_library(Assisi-Cook)
Assisi_apply_defaults(Assisi-Cook)

target_sources(Assisi-Cook
  PUBLIC
    "include/Assisi/Cook/Cooker.hpp"
    "include/Assisi/Cook/CookTree.hpp"
    "include/Assisi/Cook/PakWriter.hpp"
  PRIVATE
    "src/Cookers.cpp"
    "src/CookTree.cpp"
    "src/PakWriter.cpp"
)

target_include_directories(Assisi-Cook
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Cook
  PUBLIC
    # Core: the asset database and system the walk reads through, the blob
    # envelope, and the reflection codec every text asset cooks with.
    Assisi::Core
    # Geometry: MaterialChannels (a texture's format follows from its channel),
    # the mesh importer, and the mesh validator.
    Assisi::GeometryImport
    # Image: decode, mip generation and block compression.
    Assisi::ImageImport
    # Fonts: the rasteriser that turns a font description into an atlas.
    Assisi::FontImport
    # Screens: the markup parser and the compiler that turns a screen file into
    # the node table a game loads. The compile lives there rather than here
    # because the editor runs it in process and links no cooker.
    Assisi::MondrianImport
    # Runtime: the level loader, because cooking a level means loading it the
    # way the engine does rather than writing a second reader for the format.
    Assisi::Runtime
)

add_library(Assisi::Cook ALIAS Assisi-Cook)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
