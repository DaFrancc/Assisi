/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <Assisi/Core/AssetId.hpp>
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
        const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        const uint16_t indices[3]   = {0, 1, 2};
        std::ofstream bin(root / "triangle.bin", std::ios::binary);
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

TEST_CASE("ImportMesh: a single-material mesh degenerates to one full-range submesh")
{
    const fs::path root = WriteTriangleAssets();

    const std::expected<MeshData, MeshImportError> result = ImportMesh("triangle.gltf");
    REQUIRE(result.has_value());

    // One submesh spanning the whole index range, one LOD, one material slot —
    // the shape the flat importer's output maps onto with no behaviour change.
    REQUIRE(result->SubMeshes.size() == 1);
    CHECK(result->SubMeshes[0].IndexOffset == 0);
    CHECK(result->SubMeshes[0].IndexCount == result->Indices.size());
    CHECK(result->SubMeshes[0].MaterialSlot == 0);
    REQUIRE(result->Lods.size() == 1);
    CHECK(result->Lods[0].FirstSubMesh == 0);
    CHECK(result->Lods[0].SubMeshCount == 1);
    REQUIRE(result->Materials.size() == 1); // the primitive has no material -> spec-default slot

    // The submesh's local bounds enclose the triangle (0,0,0)-(1,0,0)-(0,1,0).
    CHECK(result->SubMeshes[0].LocalAabb.min.x == doctest::Approx(0.0f));
    CHECK(result->SubMeshes[0].LocalAabb.max.x == doctest::Approx(1.0f));
    CHECK(result->SubMeshes[0].LocalAabb.max.y == doctest::Approx(1.0f));

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

namespace
{
// One mesh, two primitives, two distinct materials. The importer must keep them
// as two submeshes (materials differ) sharing one LOD, and extract both
// materials — including the second's baseColorTexture as a virtual asset path
// resolved relative to the .gltf. The two triangles live at z=0 and z=1 so the
// per-submesh bounds are distinguishable.
constexpr std::string_view kTwoMaterialGltf = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [
    { "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 },
    { "attributes": { "POSITION": 2 }, "indices": 3, "material": 1 }
  ] } ],
  "materials": [
    { "name": "red",   "pbrMetallicRoughness": { "baseColorFactor": [1.0, 0.0, 0.0, 1.0],
      "metallicFactor": 0.25, "roughnessFactor": 0.75 } },
    { "name": "green", "pbrMetallicRoughness": { "baseColorFactor": [0.0, 1.0, 0.0, 1.0],
      "baseColorTexture": { "index": 0 } } }
  ],
  "textures": [ { "source": 0 } ],
  "images": [ { "uri": "textures/green.png" } ],
  "buffers": [ { "uri": "two.bin", "byteLength": 84 } ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0,  "byteLength": 36, "target": 34962 },
    { "buffer": 0, "byteOffset": 72, "byteLength": 6,  "target": 34963 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 36, "target": 34962 },
    { "buffer": 0, "byteOffset": 78, "byteLength": 6,  "target": 34963 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
      "min": [0.0, 0.0, 0.0], "max": [1.0, 1.0, 0.0] },
    { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" },
    { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC3",
      "min": [0.0, 0.0, 1.0], "max": [1.0, 1.0, 1.0] },
    { "bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR" }
  ]
})";

fs::path WriteTwoMaterialAssets()
{
    const fs::path root = fs::temp_directory_path() / "assisi_geometry_test_mat";
    fs::remove_all(root);
    fs::create_directories(root);

    {
        std::ofstream gltf(root / "two.gltf", std::ios::binary);
        gltf.write(kTwoMaterialGltf.data(), static_cast<std::streamsize>(kTwoMaterialGltf.size()));
    }
    {
        // Layout: pos0 (z=0), pos1 (z=1), idx0, idx1 — matching the bufferViews.
        const float pos0[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        const float pos1[9] = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f};
        const uint16_t idx0[3] = {0, 1, 2};
        const uint16_t idx1[3] = {0, 1, 2};
        std::ofstream bin(root / "two.bin", std::ios::binary);
        bin.write(reinterpret_cast<const char *>(pos0), sizeof(pos0));
        bin.write(reinterpret_cast<const char *>(pos1), sizeof(pos1));
        bin.write(reinterpret_cast<const char *>(idx0), sizeof(idx0));
        bin.write(reinterpret_cast<const char *>(idx1), sizeof(idx1));
    }

    REQUIRE(AssetSystem::SetRoot(root).has_value());
    return root;
}
} // namespace

TEST_CASE("ImportMesh: two materials become two submeshes with extracted material data")
{
    const fs::path root = WriteTwoMaterialAssets();

    // A resolver stands in for the editor's AssetDatabase: it maps the texture's
    // resolved virtual path to a known id, and records the path it was asked for.
    const Assisi::Core::AssetId kGreenId = *Assisi::Core::AssetId::Parse("11111111-1111-4111-8111-111111111111");
    std::string seenPath;
    const auto resolveId = [&](std::string_view vpath) -> Assisi::Core::AssetId
                           {
                               if (vpath == "textures/green.png")
                               {
                                   seenPath = std::string(vpath);
                                   return kGreenId;
                               }
                               return {};
                           };

    const std::expected<MeshData, MeshImportError> result = ImportMesh("two.gltf", resolveId);
    REQUIRE(result.has_value());

    CHECK(result->Vertices.size() == 6);
    CHECK(result->Indices.size() == 6);

    // Two submeshes, one LOD spanning both, two material slots.
    REQUIRE(result->SubMeshes.size() == 2);
    REQUIRE(result->Lods.size() == 1);
    CHECK(result->Lods[0].FirstSubMesh == 0);
    CHECK(result->Lods[0].SubMeshCount == 2);
    REQUIRE(result->Materials.size() == 2);

    // Submesh ranges are contiguous, disjoint, and cover the whole index buffer.
    CHECK(result->SubMeshes[0].IndexOffset == 0);
    CHECK(result->SubMeshes[0].IndexCount == 3);
    CHECK(result->SubMeshes[1].IndexOffset == 3);
    CHECK(result->SubMeshes[1].IndexCount == 3);
    CHECK(result->SubMeshes[0].MaterialSlot == 0);
    CHECK(result->SubMeshes[1].MaterialSlot == 1);

    // Per-submesh bounds separate the z=0 and z=1 triangles.
    CHECK(result->SubMeshes[0].LocalAabb.max.z == doctest::Approx(0.0f));
    CHECK(result->SubMeshes[1].LocalAabb.min.z == doctest::Approx(1.0f));

    // Material 0 factors carried through.
    const Assisi::Geometry::MaterialData &red = result->Materials[0];
    CHECK(red.Name == "red");
    CHECK(red.BaseColorFactor.r == doctest::Approx(1.0f));
    CHECK(red.BaseColorFactor.g == doctest::Approx(0.0f));
    CHECK(red.MetallicFactor == doctest::Approx(0.25f));
    CHECK(red.RoughnessFactor == doctest::Approx(0.75f));
    CHECK(red.BaseColorTexture.IsNil()); // no texture → nil id

    // Material 1's baseColorTexture resolves to a virtual path relative to the
    // .gltf, which the resolver then maps to its stable GUID.
    const Assisi::Geometry::MaterialData &green = result->Materials[1];
    CHECK(green.Name == "green");
    CHECK(green.BaseColorFactor.g == doctest::Approx(1.0f));
    CHECK(seenPath == "textures/green.png"); // resolver saw the correct resolved path
    CHECK(green.BaseColorTexture == kGreenId);

    fs::remove_all(root);
}

namespace
{
// Two nodes referencing the same one-primitive mesh; the second node's name
// carries the "_LOD1" suffix. The importer must place them in separate LODs.
constexpr std::string_view kLodGltf = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [ 0, 1 ] } ],
  "nodes": [
    { "mesh": 0, "name": "Cube" },
    { "mesh": 0, "name": "Cube_LOD1", "translation": [5.0, 0.0, 0.0] }
  ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 }, "indices": 1 } ] } ],
  "buffers": [ { "uri": "lod.bin", "byteLength": 42 } ],
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

fs::path WriteLodAssets()
{
    const fs::path root = fs::temp_directory_path() / "assisi_geometry_test_lod";
    fs::remove_all(root);
    fs::create_directories(root);

    {
        std::ofstream gltf(root / "lod.gltf", std::ios::binary);
        gltf.write(kLodGltf.data(), static_cast<std::streamsize>(kLodGltf.size()));
    }
    {
        const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        const uint16_t indices[3]   = {0, 1, 2};
        std::ofstream bin(root / "lod.bin", std::ios::binary);
        bin.write(reinterpret_cast<const char *>(positions), sizeof(positions));
        bin.write(reinterpret_cast<const char *>(indices), sizeof(indices));
    }

    REQUIRE(AssetSystem::SetRoot(root).has_value());
    return root;
}
} // namespace

TEST_CASE("ImportMesh: a _LOD<n> node-name suffix splits geometry into LODs")
{
    const fs::path root = WriteLodAssets();

    const std::expected<MeshData, MeshImportError> result = ImportMesh("lod.gltf");
    REQUIRE(result.has_value());

    // Both nodes drawn -> 6 verts, 6 indices; one submesh per LOD.
    CHECK(result->Vertices.size() == 6);
    CHECK(result->Indices.size() == 6);
    REQUIRE(result->SubMeshes.size() == 2);
    REQUIRE(result->Lods.size() == 2);

    // LOD0 (from "Cube") then LOD1 (from "Cube_LOD1"), one submesh each.
    CHECK(result->Lods[0].SubMeshCount == 1);
    CHECK(result->Lods[1].SubMeshCount == 1);
    CHECK(result->Lods[0].FirstSubMesh == 0);
    CHECK(result->Lods[1].FirstSubMesh == 1);

    // The LOD1 node is translated +5 in x, so its submesh bounds are shifted.
    const Assisi::Geometry::SubMesh &lod0 = result->SubMeshes[result->Lods[0].FirstSubMesh];
    const Assisi::Geometry::SubMesh &lod1 = result->SubMeshes[result->Lods[1].FirstSubMesh];
    CHECK(lod0.LocalAabb.max.x == doctest::Approx(1.0f));
    CHECK(lod1.LocalAabb.min.x == doctest::Approx(5.0f));

    fs::remove_all(root);
}

namespace
{
// Round-6 review C6: LOD0's node has a mesh whose only primitive lacks POSITION
// (non-drawable), while LOD1's node is drawable. The importer skips LOD0's empty
// bucket without pushing a LodRange, then pushes LOD1's — but increments
// Lods[lod=1] on a size-1 vector, an out-of-bounds write.
constexpr std::string_view kLodOobGltf = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [ 0, 1 ] } ],
  "nodes": [
    { "mesh": 0, "name": "Cube_LOD0" },
    { "mesh": 1, "name": "Cube_LOD1" }
  ],
  "meshes": [
    { "primitives": [ { "attributes": { "NORMAL": 2 }, "indices": 1 } ] },
    { "primitives": [ { "attributes": { "POSITION": 0 }, "indices": 1 } ] }
  ],
  "buffers": [ { "uri": "lodoob.bin", "byteLength": 80 } ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0,  "byteLength": 36, "target": 34962 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 6,  "target": 34963 },
    { "buffer": 0, "byteOffset": 44, "byteLength": 36, "target": 34962 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
      "min": [0.0, 0.0, 0.0], "max": [1.0, 1.0, 0.0] },
    { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" },
    { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC3" }
  ]
})";

fs::path WriteLodOobAssets()
{
    const fs::path root = fs::temp_directory_path() / "assisi_geometry_test_lodoob";
    fs::remove_all(root);
    fs::create_directories(root);
    {
        std::ofstream gltf(root / "lodoob.gltf", std::ios::binary);
        gltf.write(kLodOobGltf.data(), static_cast<std::streamsize>(kLodOobGltf.size()));
    }
    {
        const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        const uint16_t indices[3]   = {0, 1, 2};
        const uint16_t pad          = 0;
        const float normals[9]   = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
        std::ofstream bin(root / "lodoob.bin", std::ios::binary);
        bin.write(reinterpret_cast<const char *>(positions), sizeof(positions));
        bin.write(reinterpret_cast<const char *>(indices), sizeof(indices));
        bin.write(reinterpret_cast<const char *>(&pad), sizeof(pad));
        bin.write(reinterpret_cast<const char *>(normals), sizeof(normals));
    }
    REQUIRE(AssetSystem::SetRoot(root).has_value());
    return root;
}
} // namespace

TEST_CASE("ImportMesh: a non-drawable LOD bucket does not corrupt the LOD table (round-6 C6)")
{
    const fs::path root = WriteLodOobAssets();

    // Must not out-of-bounds-write the LOD table (crash) while densifying LODs.
    std::expected<MeshData, MeshImportError> result;
    CHECK_NOTHROW(result = ImportMesh("lodoob.gltf"));
    if (result.has_value())
    {
        // If it imported, the LOD table must be internally consistent: every
        // LodRange range lies within SubMeshes.
        for (const Assisi::Geometry::LodRange &lod : result->Lods)
        {
            CHECK(lod.FirstSubMesh + lod.SubMeshCount <= result->SubMeshes.size());
        }
    }

    fs::remove_all(root);
}

namespace
{
// Round-6 review M7: one triangle under a node with a NEGATIVE scale on one axis.
// The node transform has negative determinant, so baking it into the vertex
// positions mirrors the triangle and reverses its screen-space winding.
constexpr std::string_view kMirroredGltf = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "mesh": 0, "scale": [-1.0, 1.0, 1.0] } ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 }, "indices": 1 } ] } ],
  "buffers": [ { "uri": "mirror.bin", "byteLength": 42 } ],
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

fs::path WriteMirroredAssets()
{
    const fs::path root = fs::temp_directory_path() / "assisi_geometry_test_mirror";
    fs::remove_all(root);
    fs::create_directories(root);

    {
        std::ofstream gltf(root / "mirror.gltf", std::ios::binary);
        gltf.write(kMirroredGltf.data(), static_cast<std::streamsize>(kMirroredGltf.size()));
    }
    {
        const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        const uint16_t indices[3]   = {0, 1, 2};
        std::ofstream bin(root / "mirror.bin", std::ios::binary);
        bin.write(reinterpret_cast<const char *>(positions), sizeof(positions));
        bin.write(reinterpret_cast<const char *>(indices), sizeof(indices));
    }

    REQUIRE(AssetSystem::SetRoot(root).has_value());
    return root;
}
} // namespace

TEST_CASE("ImportMesh: a mirrored (negative-scale) node flips winding back to CCW (round-6 M7)")
{
    const fs::path root = WriteMirroredAssets();

    const std::expected<MeshData, MeshImportError> result = ImportMesh("mirror.gltf");
    REQUIRE(result.has_value());
    REQUIRE(result->Indices.size() == 3);

    // Confirm the negative scale actually baked in: vertex 1's x mirrored to -1.
    REQUIRE(result->Vertices[1].Position.x == doctest::Approx(-1.0f));

    // The node's negative determinant reverses the triangle's winding when baked.
    // Under back-face culling with a CCW front face, the importer must swap two
    // indices per triangle to restore CCW-front in world space -> {0, 2, 1}.
    CHECK(result->Indices[0] == 0);
    CHECK(result->Indices[1] == 2); // importer leaves the reversed winding {0,1,2}
    CHECK(result->Indices[2] == 1);

    fs::remove_all(root);
}
