add_library(Assisi-Math)
Assisi_apply_defaults(Assisi-Math)

target_sources(Assisi-Math
  PRIVATE
    "src/Math.cpp"
)

target_include_directories(Assisi-Math
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_library(Assisi::Math ALIAS Assisi-Math)
