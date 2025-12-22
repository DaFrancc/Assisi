Assisi_add_module(Assisi-Core)

target_sources(Assisi-Core
  PRIVATE
    "src/Core.cpp"
  PUBLIC
    include/Assisi/Core/Core.hpp
)

Assisi_module_include(Assisi-Core)

add_library(Assisi::Core ALIAS Assisi-Core)
