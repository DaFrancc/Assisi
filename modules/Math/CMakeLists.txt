add_library(Assisi-Math)
Assisi_apply_defaults(Assisi-Math)

target_sources(Assisi-Math
  PRIVATE
    "src/Math.cpp"
)

target_include_directories(Assisi-Math
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# GLM.hpp is a public header that includes <glm/glm.hpp>, so Math owns the glm
# dependency and propagates its include path to every consumer — not only the
# ones that also link Render.
target_link_libraries(Assisi-Math
  PUBLIC
    glm::glm
)

add_library(Assisi::Math ALIAS Assisi-Math)
