add_library(Assisi-Geometry)
Assisi_apply_defaults(Assisi-Geometry)

target_sources(Assisi-Geometry
  PUBLIC
    "include/Assisi/Geometry/MeshData.hpp"
    "include/Assisi/Geometry/Bounds.hpp"
    "include/Assisi/Geometry/MaterialData.hpp"
    "include/Assisi/Geometry/DefaultMeshes.hpp"
    "include/Assisi/Geometry/MeshImporter.hpp"
  PRIVATE
    "src/MeshImporter.cpp"
)

target_include_directories(Assisi-Geometry
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Geometry
  PUBLIC
    # Core: AssetSystem (all mesh-file reads go through it) and Logger.
    Assisi::Core
    # Math: glm, used by MeshData's vertex layout.
    Assisi::Math
  PRIVATE
    # fastgltf is an implementation detail of the glTF backend; it must not leak
    # to Geometry's consumers, so it is linked PRIVATE.
    fastgltf::fastgltf
)

add_library(Assisi::Geometry ALIAS Assisi-Geometry)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
