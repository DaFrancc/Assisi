add_library(Assisi-Image)
Assisi_apply_defaults(Assisi-Image)

target_sources(Assisi-Image
  PUBLIC
    "include/Assisi/Image/Image.hpp"
    "include/Assisi/Image/Decode.hpp"
    "include/Assisi/Image/Compress.hpp"
  PRIVATE
    "src/Image.cpp"
    "src/Decode.cpp"
    "src/Compress.cpp"
)

target_include_directories(Assisi-Image
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Image
  PUBLIC
    # Core: AssetSystem (every image read goes through it), Errors and Logger.
    Assisi::Core
  PRIVATE
    # The codecs are implementation details of this module: a consumer asks for
    # pixels and must not inherit stb's or libwebp's headers to get them.
    stb::stb
    webp
    webpdemux
    bc7enc::bc7enc
)

add_library(Assisi::Image ALIAS Assisi-Image)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
