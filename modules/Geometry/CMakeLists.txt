add_library(Assisi-Geometry)
Assisi_apply_defaults(Assisi-Geometry)

target_sources(Assisi-Geometry
  PUBLIC
    "include/Assisi/Geometry/CookedMesh.hpp"
    "include/Assisi/Geometry/MeshData.hpp"
    "include/Assisi/Geometry/Bounds.hpp"
    "include/Assisi/Geometry/MaterialData.hpp"
    "include/Assisi/Geometry/MaterialChannels.hpp"
    "include/Assisi/Geometry/DefaultMeshes.hpp"
    "include/Assisi/Geometry/MeshValidate.hpp"
    "include/Assisi/Geometry/MaterialFile.hpp"
  PRIVATE
    "src/CookedMesh.cpp"
    "src/MeshValidate.cpp"
    "src/MaterialFile.cpp"
)

target_include_directories(Assisi-Geometry
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(Assisi-Geometry
  PUBLIC
    # Core: Logger and the bit streams a cooked mesh reads through.
    Assisi::Core
    # Math: glm, used by MeshData's vertex layout.
    Assisi::Math
    # Image: PixelFormat and ColorSpace, which MaterialChannels names — a
    # channel's on-GPU format is a property of what the channel is for.
    Assisi::Image
)

add_library(Assisi::Geometry ALIAS Assisi-Geometry)

# Importing glTF and exploding its materials: what the editor and the cook do to
# source meshes, and a shipped game never does. A library of its own so the
# game's link cannot reach fastgltf at all — see Assisi-ImageImport for why
# leaving it to the linker's object selection is not enough.
add_library(Assisi-GeometryImport)
Assisi_apply_defaults(Assisi-GeometryImport)

target_sources(Assisi-GeometryImport
  PUBLIC
    "include/Assisi/Geometry/MeshImporter.hpp"
    "include/Assisi/Geometry/AssetImport.hpp"
  PRIVATE
    "src/MeshImporter.cpp"
    "src/AssetImport.cpp"
)

target_link_libraries(Assisi-GeometryImport
  PUBLIC
    Assisi::Geometry
  PRIVATE
    # fastgltf is an implementation detail of the glTF backend; it must not leak
    # to this library's consumers, so it is linked PRIVATE.
    fastgltf::fastgltf
)

add_library(Assisi::GeometryImport ALIAS Assisi-GeometryImport)

# MaterialData is an AASSET: reflectgen emits its AssetTypeRegistry registration
# (serialize/deserialize for .amat). The generated OBJECT library inherits
# Geometry's include paths (and Core -> nlohmann_json) transitively.
assisi_reflect(
  TARGET Assisi-Geometry
  HEADERS
    include/Assisi/Geometry/MaterialData.hpp
)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
