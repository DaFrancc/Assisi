add_library(Assisi-Core)
Assisi_apply_defaults(Assisi-Core)

target_sources(Assisi-Core
  PRIVATE
    "src/Core.cpp"
    "src/AssetSystem.cpp"
    "src/Logger.cpp"
    "src/Sinks.cpp"
  PUBLIC
    "include/Assisi/Core/Core.hpp"
    "include/Assisi/Core/AssetSystem.hpp"
    "include/Assisi/Core/Logger.hpp"
    "include/Assisi/Core/Sinks.hpp"
    "include/Assisi/Prelude.hpp"
)

target_include_directories(Assisi-Core
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_library(Assisi::Core ALIAS Assisi-Core)
