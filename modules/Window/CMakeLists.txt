add_library(Assisi-Window)
Assisi_apply_defaults(Assisi-Window)

target_sources(Assisi-Window
  PUBLIC
    "include/Assisi/Window/ActionMap.hpp"
    "include/Assisi/Window/GlfwLibrary.hpp"
    "include/Assisi/Window/InputBindings.hpp"
    "include/Assisi/Window/InputContext.hpp"
    "include/Assisi/Window/InputEvent.hpp"
    "include/Assisi/Window/Key.hpp"
    "include/Assisi/Window/WindowContext.hpp"
  PRIVATE
    "src/ActionMap.cpp"
    "src/GlfwLibrary.cpp"
    "src/InputBindings.cpp"
    "src/InputContext.cpp"
    "src/WindowContext.cpp"
)

target_include_directories(Assisi-Window
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Window
  PUBLIC
    Assisi::Core
    Assisi::Deps
  PRIVATE
    nlohmann_json::nlohmann_json
)

# InputBindings is an AASSET: reflectgen emits its AssetTypeRegistry
# registration, which is what reads config/input.json. The generated OBJECT
# library inherits Window's include paths (and Core -> nlohmann_json)
# transitively.
assisi_reflect(
  TARGET Assisi-Window
  HEADERS
    include/Assisi/Window/InputBindings.hpp
)

add_library(Assisi::Window ALIAS Assisi-Window)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()