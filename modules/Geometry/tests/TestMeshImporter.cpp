/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Geometry/MeshImporter.hpp>

#include "LogCapture.hpp"

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

    // One submesh spanning the whole index range, one LOD, one material slot.
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
// bucket, so the LOD table has to be pre-sized by distinct LOD count — indexing
// it by the dense lod value is otherwise an out-of-bounds write.
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
    CHECK(result->Indices[1] == 2);
    CHECK(result->Indices[2] == 1);

    fs::remove_all(root);
}

namespace
{
// One triangle with one material carrying every KHR_materials_* extension the
// OpenPBR base layer can express, alongside three it cannot. The unsupported
// three are listed in extensionsUsed the way a real exporter writes them, which
// is what the importer reads to warn: the parser's extension mask strips their
// material blocks, so nothing else is left to notice them by.
constexpr std::string_view kKhrGltf = R"({
  "asset": { "version": "2.0" },
  "extensionsUsed": [
    "KHR_materials_ior", "KHR_materials_specular", "KHR_materials_emissive_strength",
    "KHR_materials_transmission", "KHR_materials_clearcoat", "KHR_materials_sheen"
  ],
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 } ] } ],
  "materials": [ {
    "name": "lacquer",
    "emissiveFactor": [0.5, 0.25, 0.0],
    "extensions": {
      "KHR_materials_ior": { "ior": 1.8 },
      "KHR_materials_specular": { "specularFactor": 0.6, "specularColorFactor": [0.9, 0.8, 0.7] },
      "KHR_materials_emissive_strength": { "emissiveStrength": 4.0 },
      "KHR_materials_transmission": { "transmissionFactor": 1.0 },
      "KHR_materials_clearcoat": { "clearcoatFactor": 1.0 },
      "KHR_materials_sheen": { "sheenColorFactor": [1.0, 1.0, 1.0] }
    }
  } ],
  "buffers": [ { "uri": "khr.bin", "byteLength": 42 } ],
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

// A material extension the engine cannot express, marked *required* — which is
// what a real exporter writes for transmission. glTF says a renderer that cannot
// honour a required extension should refuse the file, and fastgltf enforces that
// by aborting the parse for anything outside the parser's mask. Masking to only
// the extensions we read would therefore lose a whole model over one material
// parameter it was never going to use.
constexpr std::string_view kRequiredUnsupportedGltf = R"({
  "asset": { "version": "2.0" },
  "extensionsUsed": [ "KHR_materials_transmission" ],
  "extensionsRequired": [ "KHR_materials_transmission" ],
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 } ] } ],
  "materials": [ { "name": "glass", "extensions": {
    "KHR_materials_transmission": { "transmissionFactor": 1.0 } } } ],
  "buffers": [ { "uri": "khr.bin", "byteLength": 42 } ],
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

// One material whose KHR_materials_specular carries a texture as well as its
// factors. There is no specular texture channel to put it in.
constexpr std::string_view kSpecularTextureGltf = R"({
  "asset": { "version": "2.0" },
  "extensionsUsed": [ "KHR_materials_specular" ],
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 } ] } ],
  "materials": [ { "name": "brushed", "extensions": { "KHR_materials_specular": {
    "specularFactor": 0.3, "specularTexture": { "index": 0 } } } } ],
  "textures": [ { "source": 0 } ],
  "images": [ { "uri": "spec.png" } ],
  "buffers": [ { "uri": "khr.bin", "byteLength": 42 } ],
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

// The same triangle with one material whose only extension is
// KHR_materials_ior, carrying @p iorLiteral verbatim.
std::string MakeIorGltf(std::string_view iorLiteral)
{
    constexpr std::string_view kHead = R"({
  "asset": { "version": "2.0" },
  "extensionsUsed": [ "KHR_materials_ior" ],
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 } ] } ],
  "materials": [ { "name": "refractive", "extensions": { "KHR_materials_ior": { "ior": )";
    constexpr std::string_view kTail = R"( } } } ],
  "buffers": [ { "uri": "khr.bin", "byteLength": 42 } ],
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
    return std::string{kHead} + std::string{iorLiteral} + std::string{kTail};
}

// The same triangle with one material whose alphaMode is @p mode, plus whatever
// @p extraKeys adds (an alphaCutoff, or nothing).
std::string MakeAlphaGltf(std::string_view mode, std::string_view extraKeys)
{
    constexpr std::string_view kHead = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 } ] } ],
  "materials": [ { "name": "foliage", "alphaMode": ")";
    constexpr std::string_view kTail = R"( } ],
  "buffers": [ { "uri": "khr.bin", "byteLength": 42 } ],
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
    return std::string{kHead} + std::string{mode} + "\"" + std::string{extraKeys} + std::string{kTail};
}

fs::path WriteKhrAssets(std::string_view gltfText)
{
    const fs::path root = fs::temp_directory_path() / "assisi_geometry_test_khr";
    fs::remove_all(root);
    fs::create_directories(root);

    {
        std::ofstream gltf(root / "khr.gltf", std::ios::binary);
        gltf.write(gltfText.data(), static_cast<std::streamsize>(gltfText.size()));
    }
    {
        const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        const uint16_t indices[3]   = {0, 1, 2};
        std::ofstream bin(root / "khr.bin", std::ios::binary);
        bin.write(reinterpret_cast<const char *>(positions), sizeof(positions));
        bin.write(reinterpret_cast<const char *>(indices), sizeof(indices));
    }

    REQUIRE(AssetSystem::SetRoot(root).has_value());
    return root;
}
} // namespace

TEST_CASE("ImportMesh: KHR_materials ior/specular/emissive_strength reach the OpenPBR fields")
{
    const fs::path root = WriteKhrAssets(kKhrGltf);

    const std::expected<MeshData, MeshImportError> result = ImportMesh("khr.gltf");
    REQUIRE(result.has_value());
    REQUIRE(result->Materials.size() == 1);
    const Assisi::Geometry::MaterialData &material = result->Materials[0];

    CHECK(material.SpecularIor == doctest::Approx(1.8f));
    CHECK(material.SpecularWeight == doctest::Approx(0.6f));
    CHECK(material.SpecularColor.r == doctest::Approx(0.9f));
    CHECK(material.SpecularColor.g == doctest::Approx(0.8f));
    CHECK(material.SpecularColor.b == doctest::Approx(0.7f));

    // emissiveStrength scales the emissive factor, and past 1: an HDR emissive
    // is the entire point of the extension.
    CHECK(material.EmissiveFactor.r == doctest::Approx(2.0f));
    CHECK(material.EmissiveFactor.g == doctest::Approx(1.0f));
    CHECK(material.EmissiveFactor.b == doctest::Approx(0.0f));

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: a KHR_materials extension the engine cannot express is named in a warning")
{
    const fs::path root = WriteKhrAssets(kKhrGltf);
    const Assisi::Tests::LogCapture log;

    REQUIRE(ImportMesh("khr.gltf").has_value());

    CHECK(log.Mentions("KHR_materials_transmission"));
    CHECK(log.Mentions("KHR_materials_clearcoat"));
    CHECK(log.Mentions("KHR_materials_sheen"));

    // The three that are mapped are not dropped, so they must not be warned about.
    CHECK_FALSE(log.Mentions("KHR_materials_ior"));
    CHECK_FALSE(log.Mentions("KHR_materials_specular"));
    CHECK_FALSE(log.Mentions("KHR_materials_emissive_strength"));

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: an ior outside SpecularIor's authoring range is imported as authored")
{
    // SpecularIor's [1, 3] is a slider range, not a validity bound: F0 =
    // ((ior-1)/(ior+1))^2 is continuous and within [0, 1] for every positive
    // ior, so narrowing either end here would only discard what was authored.
    // glTF puts no ceiling on ior at all, and its ior of 0 is the special case
    // requiring a Fresnel of 1 — which is what that expression returns for it.
    const float iors[] = {0.0f, 0.4f, 4.0f};
    for (const float authored : iors)
    {
        CAPTURE(authored);
        const fs::path root = WriteKhrAssets(MakeIorGltf(std::to_string(authored)));
        const Assisi::Tests::LogCapture log;

        const std::expected<MeshData, MeshImportError> result = ImportMesh("khr.gltf");
        REQUIRE(result.has_value());
        REQUIRE(result->Materials.size() == 1);

        CHECK(result->Materials[0].SpecularIor == doctest::Approx(authored));
        CHECK_FALSE(log.Mentions("KHR_materials_ior")); // authored, not salvaged

        fs::remove_all(root);
    }
}

TEST_CASE("ImportMesh: a required unsupported extension warns instead of failing the import")
{
    const fs::path root = WriteKhrAssets(kRequiredUnsupportedGltf);
    const Assisi::Tests::LogCapture log;

    // The geometry is the point: one unsupported material parameter must not cost
    // the whole model. A parse failure here also names nothing, because the
    // warning is read off the asset and a failed parse leaves no asset to read.
    const std::expected<MeshData, MeshImportError> result = ImportMesh("khr.gltf");
    REQUIRE(result.has_value());
    CHECK(result->Vertices.size() == 3);

    CHECK(log.Mentions("KHR_materials_transmission"));

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: a KHR_materials_specular texture is named as dropped, factors kept")
{
    const fs::path root = WriteKhrAssets(kSpecularTextureGltf);
    const Assisi::Tests::LogCapture log;

    const std::expected<MeshData, MeshImportError> result = ImportMesh("khr.gltf");
    REQUIRE(result.has_value());
    REQUIRE(result->Materials.size() == 1);

    // The texture is what is unsupported, not the extension: the factors it
    // shipped alongside still land.
    CHECK(result->Materials[0].SpecularWeight == doctest::Approx(0.3f));
    CHECK(log.Mentions("KHR_materials_specular"));

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: alphaMode MASK imports as a cutout with its authored cutoff")
{
    const fs::path root = WriteKhrAssets(MakeAlphaGltf("MASK", R"(, "alphaCutoff": 0.25)"));
    const Assisi::Tests::LogCapture log;

    const std::expected<MeshData, MeshImportError> result = ImportMesh("khr.gltf");
    REQUIRE(result.has_value());
    REQUIRE(result->Materials.size() == 1);

    CHECK(result->Materials[0].Alpha == Assisi::Geometry::AlphaMode::Mask);
    CHECK(result->Materials[0].AlphaCutoff == doctest::Approx(0.25f));
    // Nothing was dropped, so nothing may be warned about.
    CHECK_FALSE(log.Mentions("alpha"));

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: alphaMode MASK without an explicit cutoff takes the glTF default")
{
    // glTF's alphaCutoff defaults to 0.5, and exporters routinely omit it. Taking
    // the engine's own default instead would only agree by coincidence.
    const fs::path root = WriteKhrAssets(MakeAlphaGltf("MASK", ""));

    const std::expected<MeshData, MeshImportError> result = ImportMesh("khr.gltf");
    REQUIRE(result.has_value());
    REQUIRE(result->Materials.size() == 1);

    CHECK(result->Materials[0].Alpha == Assisi::Geometry::AlphaMode::Mask);
    CHECK(result->Materials[0].AlphaCutoff == doctest::Approx(0.5f));

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: alphaMode BLEND still imports as opaque, and says so")
{
    // There is no blended pass yet, so BLEND has nowhere to go. Importing it as a
    // cutout would be worse than opaque: it would punch holes in a surface the
    // author meant to see through smoothly.
    const fs::path root = WriteKhrAssets(MakeAlphaGltf("BLEND", ""));
    const Assisi::Tests::LogCapture log;

    const std::expected<MeshData, MeshImportError> result = ImportMesh("khr.gltf");
    REQUIRE(result.has_value());
    REQUIRE(result->Materials.size() == 1);

    CHECK(result->Materials[0].Alpha == Assisi::Geometry::AlphaMode::Opaque);
    CHECK(log.Mentions("BLEND"));

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: doubleSided is imported rather than dropped")
{
    // The importer used to warn and discard this. Dropping it is what made a
    // cutout hole look through the object to the background instead of its inside.
    const fs::path root = WriteKhrAssets(MakeAlphaGltf("MASK", R"(, "doubleSided": true)"));
    const Assisi::Tests::LogCapture log;

    const std::expected<MeshData, MeshImportError> result = ImportMesh("khr.gltf");
    REQUIRE(result.has_value());
    REQUIRE(result->Materials.size() == 1);

    CHECK(result->Materials[0].DoubleSided == true);
    CHECK_FALSE(log.Mentions("single-sided"));

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: a material that does not say doubleSided stays single-sided")
{
    // glTF's default is false, and a closed opaque mesh should not start paying
    // fill for faces it will never show.
    const fs::path root = WriteKhrAssets(MakeAlphaGltf("OPAQUE", ""));

    const std::expected<MeshData, MeshImportError> result = ImportMesh("khr.gltf");
    REQUIRE(result.has_value());
    REQUIRE(result->Materials.size() == 1);

    CHECK(result->Materials[0].DoubleSided == false);

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: a negative ior is refused and named")
{
    // The one value the shader cannot take: F0's denominator (ior + 1) is zero
    // at -1, and no index of refraction is negative to begin with.
    const fs::path root = WriteKhrAssets(MakeIorGltf("-1.0"));
    const Assisi::Tests::LogCapture log;

    const std::expected<MeshData, MeshImportError> result = ImportMesh("khr.gltf");
    REQUIRE(result.has_value());
    REQUIRE(result->Materials.size() == 1);

    CHECK(result->Materials[0].SpecularIor == doctest::Approx(1.5f)); // the default, untouched
    CHECK(log.Mentions("KHR_materials_ior"));

    fs::remove_all(root);
}
