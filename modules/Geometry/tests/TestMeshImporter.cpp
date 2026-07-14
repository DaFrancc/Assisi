/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Geometry/MeshImporter.hpp>

using Assisi::Core::AssetSystem;
using Assisi::Geometry::ImportMesh;
using Assisi::Geometry::MeshData;
using Assisi::Geometry::MeshImportError;

namespace fs = std::filesystem;

namespace
{
// A minimal glTF 2.0 document: one triangle whose geometry lives in an *external*
// buffer file ("triangle.bin"). Loading it exercises the path that matters most
// here — fastgltf leaving the buffer as a URI and the importer reading the
// sibling through AssetSystem rather than the raw filesystem.
constexpr std::string_view kTriangleGltf = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 }, "indices": 1 } ] } ],
  "buffers": [ { "uri": "triangle.bin", "byteLength": 42 } ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0,  "byteLength": 36, "target": 34962 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 6,  "target": 34963 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
      "min": [0.0, 0.0, 0.0], "max": [1.0, 1.0, 0.0] },
    { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
  ]
})";

// Writes kTriangleGltf and its external buffer into a fresh temp asset root and
// points AssetSystem at it. Returns the root so the caller can clean it up.
fs::path WriteTriangleAssets()
{
    const fs::path root = fs::temp_directory_path() / "assisi_geometry_test";
    fs::remove_all(root);
    fs::create_directories(root);

    {
        std::ofstream gltf(root / "triangle.gltf", std::ios::binary);
        gltf.write(kTriangleGltf.data(), static_cast<std::streamsize>(kTriangleGltf.size()));
    }
    {
        const float    positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        const uint16_t indices[3]   = {0, 1, 2};
        std::ofstream  bin(root / "triangle.bin", std::ios::binary);
        bin.write(reinterpret_cast<const char *>(positions), sizeof(positions));
        bin.write(reinterpret_cast<const char *>(indices), sizeof(indices));
    }

    REQUIRE(AssetSystem::SetRoot(root).has_value());
    return root;
}
} // namespace

TEST_CASE("ImportMesh: loads a glTF triangle via an AssetSystem-mediated external buffer")
{
    const fs::path root = WriteTriangleAssets();

    const std::expected<MeshData, MeshImportError> result = ImportMesh("triangle.gltf");
    REQUIRE(result.has_value());

    CHECK(result->Vertices.size() == 3);
    REQUIRE(result->Indices.size() == 3);
    CHECK(result->Indices[0] == 0);
    CHECK(result->Indices[1] == 1);
    CHECK(result->Indices[2] == 2);

    // Node transform is identity, so positions come through verbatim.
    CHECK(result->Vertices[0].Position.x == doctest::Approx(0.0f));
    CHECK(result->Vertices[1].Position.x == doctest::Approx(1.0f));
    CHECK(result->Vertices[2].Position.y == doctest::Approx(1.0f));

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: rejects unsupported extensions")
{
    const std::expected<MeshData, MeshImportError> result = ImportMesh("model.fbx");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshImportError::UnsupportedFormat);
}

TEST_CASE("ImportMesh: reports a read failure for a missing glTF")
{
    const fs::path root = WriteTriangleAssets();

    const std::expected<MeshData, MeshImportError> result = ImportMesh("does_not_exist.gltf");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshImportError::ReadFailed);

    fs::remove_all(root);
}
