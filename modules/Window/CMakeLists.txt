add_library(Assisi-Window)
Assisi_apply_defaults(Assisi-Window)

target_sources(Assisi-Window
  PRIVATE
    "src/Window.cpp"
)

target_include_directories(Assisi-Window
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Window
  PUBLIC
    Assisi::Core
    Assisi::Deps
)

add_library(Assisi::Window ALIAS Assisi-Window)
