add_library(Assisi-Image)
Assisi_apply_defaults(Assisi-Image)

target_sources(Assisi-Image
  PUBLIC
    "include/Assisi/Image/Image.hpp"
    "include/Assisi/Image/CookedTexture.hpp"
  PRIVATE
    "src/CookedTexture.cpp"
    "src/Image.cpp"
)

target_include_directories(Assisi-Image
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Image
  PUBLIC
    # Core: Errors, Logger and the bit streams a cooked texture reads through.
    Assisi::Core
)

add_library(Assisi::Image ALIAS Assisi-Image)

# Decoding image files and block-compressing them: what the editor and the cook
# do to source images, and a shipped game never does. A library of its own so the
# game's link cannot reach the codecs at all. Leaving them in Assisi-Image and
# relying on the linker to skip unreferenced objects is not enough: an archive
# member is also pulled in to satisfy a symbol it happens to share with anything
# already linked, and that is decided by link order and optimisation level.
add_library(Assisi-ImageImport)
Assisi_apply_defaults(Assisi-ImageImport)

target_sources(Assisi-ImageImport
  PUBLIC
    "include/Assisi/Image/Decode.hpp"
    "include/Assisi/Image/Compress.hpp"
  PRIVATE
    "src/Decode.cpp"
    "src/Compress.cpp"
)

target_link_libraries(Assisi-ImageImport
  PUBLIC
    # Image: the DecodedImage every decode produces, and through it Core, whose
    # AssetSystem every image read goes through.
    Assisi::Image
  PRIVATE
    # The codecs are implementation details of this module: a consumer asks for
    # pixels and must not inherit stb's or libwebp's headers to get them.
    stb::stb
    webp
    webpdemux
    bc7enc::bc7enc
)

add_library(Assisi::ImageImport ALIAS Assisi-ImageImport)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
