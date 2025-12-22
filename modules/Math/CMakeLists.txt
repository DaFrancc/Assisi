Assisi_add_module(Assisi-Math)

target_sources(Assisi-Math
  PRIVATE
  src/Math.cpp
)

target_include_directories(Assisi-Math
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

Assisi_module_include(Assisi-Math)

add_library(Assisi::Math ALIAS Assisi-Math)
