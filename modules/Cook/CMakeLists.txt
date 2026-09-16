add_library(Assisi-Cook)
Assisi_apply_defaults(Assisi-Cook)

target_sources(Assisi-Cook
  PUBLIC
    "include/Assisi/Cook/Cooker.hpp"
    "include/Assisi/Cook/CookTree.hpp"
  PRIVATE
    "src/Cookers.cpp"
    "src/CookTree.cpp"
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
    Assisi::Geometry
    # Image: decode, mip generation and block compression.
    Assisi::Image
    # Runtime: the level loader, because cooking a level means loading it the
    # way the engine does rather than writing a second reader for the format.
    Assisi::Runtime
)

add_library(Assisi::Cook ALIAS Assisi-Cook)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
