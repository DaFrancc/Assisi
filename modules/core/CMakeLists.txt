Assisi_add_module(Assisi-Core)

target_sources(Assisi-Core
  PRIVATE
    "src/Core.cpp"
    "src/AssetSystem.cpp"
  PUBLIC
    "include/Assisi/Core/Core.hpp"
    "include/Assisi/Core/AssetSystem.hpp"
)

Assisi_module_include(Assisi-Core)

add_library(Assisi::Core ALIAS Assisi-Core)
