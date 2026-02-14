add_library(Assisi-ECS)
Assisi_apply_defaults(Assisi-ECS)

target_sources(Assisi-ECS
  PRIVATE
    "src/ECS.cpp"
)

target_include_directories(Assisi-ECS
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-ECS
  PUBLIC
    Assisi::Core
    Assisi::Math
    Assisi::Render
)

add_library(Assisi::ECS ALIAS Assisi-ECS)
