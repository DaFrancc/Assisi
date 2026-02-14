add_library(Assisi-Runtime)
Assisi_apply_defaults(Assisi-Runtime)

target_sources(Assisi-Runtime
  PRIVATE
    "src/Camera.cpp"
    "src/SpawnSystem.cpp"
)

target_include_directories(Assisi-Runtime
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Runtime
  PUBLIC
    Assisi::Core
    Assisi::Math
    Assisi::Render
)

add_library(Assisi::Runtime ALIAS Assisi-Runtime)
