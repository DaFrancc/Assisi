add_library(Assisi-Core)
Assisi_apply_defaults(Assisi-Core)

target_sources(Assisi-Core
  PRIVATE
    "src/AssetSystem.cpp"
    "src/ComponentRegistry.cpp"
    "src/EventQueue.cpp"
    "src/Logger.cpp"
    "src/Sinks.cpp"
  PUBLIC
    "include/Assisi/Core/AssetSystem.hpp"
    "include/Assisi/Core/Errors.hpp"
    "include/Assisi/Core/EventQueue.hpp"
    "include/Assisi/Core/Logger.hpp"
    "include/Assisi/Core/Sinks.hpp"
    "include/Assisi/Core/Reflect/Annotations.hpp"
    "include/Assisi/Core/Reflect/FieldMeta.hpp"
    "include/Assisi/Core/Reflect/ComponentMeta.hpp"
    "include/Assisi/Core/Reflect/ComponentRegistry.hpp"
    "include/Assisi/Prelude.hpp"
    "include/Assisi/ShortNames.hpp"
)

target_include_directories(Assisi-Core
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Core
  PUBLIC
    nlohmann_json::nlohmann_json
)

add_library(Assisi::Core ALIAS Assisi-Core)