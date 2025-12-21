assisi_add_module(assisi-core)

target_sources(assisi-core
  PRIVATE
    src/core.cpp
  PUBLIC
    include/assisi/core/core.hpp
)

assisi_module_include(assisi-core)

add_library(assisi::core ALIAS assisi-core)
