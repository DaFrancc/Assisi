add_library(Assisi-Core)
Assisi_apply_defaults(Assisi-Core)

target_sources(Assisi-Core
  PRIVATE
    "src/AssetSystem.cpp"
    "src/AssetTypeRegistry.cpp"
    "src/Assert.cpp"
    "src/ComponentRegistry.cpp"
    "src/Logger.cpp"
    "src/Platform.cpp"
    "src/Sinks.cpp"
  PUBLIC
    "include/Assisi/Core/Assert.hpp"
    "include/Assisi/Core/AssetPath.hpp"
    "include/Assisi/Core/AssetSystem.hpp"
    "include/Assisi/Core/Errors.hpp"
    "include/Assisi/Core/EventQueue.hpp"
    "include/Assisi/Core/Logger.hpp"
    "include/Assisi/Core/Sinks.hpp"
    "include/Assisi/Core/StringHash.hpp"
    "include/Assisi/Core/TrivialString.hpp"
    "include/Assisi/Core/Reflect/Annotations.hpp"
    "include/Assisi/Core/Reflect/FieldMeta.hpp"
    "include/Assisi/Core/Reflect/ComponentMeta.hpp"
    "include/Assisi/Core/Reflect/ComponentRegistry.hpp"
    "include/Assisi/Core/Reflect/AssetTypeMeta.hpp"
    "include/Assisi/Core/Reflect/AssetTypeRegistry.hpp"
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

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()