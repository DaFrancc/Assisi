/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetSidecar.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Geometry/AssetImport.hpp>
#include <Assisi/Geometry/MaterialFile.hpp>

using Assisi::Core::AssetId;
using Assisi::Core::AssetSidecar;
using Assisi::Core::AssetSystem;
using Assisi::Core::DeserializeSidecar;
using Assisi::Core::MintAssetId;
using Assisi::Core::SerializeSidecar;
using Assisi::Geometry::DeserializeMaterial;
using Assisi::Geometry::ExplodeGltfMaterials;

namespace fs = std::filesystem;

namespace
{
// A one-triangle glTF whose single primitive carries a named material with a
// base-color factor and an external base-color texture ("wood.png"). Exploding
// it must produce one `.amat` capturing both, plus the slot→material manifest.
constexpr std::string_view kMaterialGltf = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 } ] } ],
  "materials": [ {
    "name": "Wood",
    "pbrMetallicRoughness": {
      "baseColorFactor": [0.8, 0.6, 0.4, 1.0],
      "baseColorTexture": { "index": 0 }
    }
  } ],
  "textures": [ { "source": 0 } ],
  "images": [ { "uri": "wood.png" } ],
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

std::string ReadFile(const fs::path &path)
{
    std::ifstream     stream(path, std::ios::binary);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// Writes the glTF, its external buffer, a stand-in texture, and the glTF's
// `.aast` sidecar (as the reconcile pass would have, with @p gltfId). Points
// AssetSystem at the fresh root and returns it.
fs::path WriteMaterialAssets(AssetId gltfId)
{
    const fs::path  root = fs::temp_directory_path() / "assisi_assetimport_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    {
        std::ofstream gltf(root / "model.gltf", std::ios::binary);
        gltf.write(kMaterialGltf.data(), static_cast<std::streamsize>(kMaterialGltf.size()));
    }
    {
        const float    positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        const uint16_t indices[3]   = {0, 1, 2};
        std::ofstream  bin(root / "triangle.bin", std::ios::binary);
        bin.write(reinterpret_cast<const char *>(positions), sizeof(positions));
        bin.write(reinterpret_cast<const char *>(indices), sizeof(indices));
    }
    {
        std::ofstream png(root / "wood.png", std::ios::binary);
        png << "PNG-BYTES";
    }
    {
        std::ofstream aast(root / "model.gltf.aast", std::ios::binary);
        const std::string text = SerializeSidecar(AssetSidecar{.guid = gltfId});
        aast.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    REQUIRE(AssetSystem::SetRoot(root).has_value());
    return root;
}
} // namespace

TEST_CASE("ExplodeGltfMaterials writes a .amat + sidecar and the glTF manifest")
{
    const AssetId gltfId = MintAssetId();
    const AssetId woodId = MintAssetId();
    const fs::path root  = WriteMaterialAssets(gltfId);

    // Resolver stands in for the AssetDatabase: the base-color texture resolves
    // to its GUID; anything else is nil (factor-only).
    const auto resolve = [woodId](std::string_view path) -> AssetId
    { return path == "wood.png" ? woodId : AssetId{}; };

    const std::expected<std::size_t, Assisi::Geometry::MeshImportError> count =
        ExplodeGltfMaterials("model.gltf", resolve);
    REQUIRE(count.has_value());
    CHECK(*count == 1);

    // The material was materialized as a sibling `.amat` (+ its own sidecar).
    const fs::path amat = root / "model_Wood.amat";
    REQUIRE(fs::exists(amat));
    REQUIRE(fs::exists(root / "model_Wood.amat.aast"));

    // The glTF sidecar now records slot 0 → the exploded material's GUID.
    const std::expected<AssetSidecar, Assisi::Core::AssetSidecarError> gltfSidecar =
        DeserializeSidecar(ReadFile(root / "model.gltf.aast"));
    REQUIRE(gltfSidecar.has_value());
    CHECK(gltfSidecar->guid == gltfId); // identity preserved (reconcile-not-clobber)
    REQUIRE(gltfSidecar->subAssets.size() == 1);
    CHECK(gltfSidecar->subAssets[0].slot == 0);

    const std::expected<AssetSidecar, Assisi::Core::AssetSidecarError> amatSidecar =
        DeserializeSidecar(ReadFile(root / "model_Wood.amat.aast"));
    REQUIRE(amatSidecar.has_value());
    CHECK(gltfSidecar->subAssets[0].material == amatSidecar->guid); // manifest points at the real file

    // The written material carries the glTF's factors and the resolved channel.
    const std::expected<Assisi::Geometry::MaterialData, Assisi::Geometry::MaterialFileError> material =
        DeserializeMaterial(ReadFile(amat));
    REQUIRE(material.has_value());
    CHECK(material->BaseColorFactor.x == doctest::Approx(0.8f));
    CHECK(material->BaseColorFactor.z == doctest::Approx(0.4f));
    CHECK(material->BaseColorTexture == woodId);

    fs::remove_all(root);
}

TEST_CASE("ExplodeGltfMaterials is idempotent: an existing .amat keeps its id and bytes")
{
    const AssetId  gltfId = MintAssetId();
    const fs::path root   = WriteMaterialAssets(gltfId);
    const auto     resolve = [](std::string_view) -> AssetId { return {}; };

    REQUIRE(ExplodeGltfMaterials("model.gltf", resolve).has_value());
    const std::string firstAmat   = ReadFile(root / "model_Wood.amat");
    const std::string firstSidecar = ReadFile(root / "model_Wood.amat.aast");

    // A second explosion reuses the existing material file verbatim — no remint,
    // no rewrite (reconcile-not-clobber over a materialized child).
    REQUIRE(ExplodeGltfMaterials("model.gltf", resolve).has_value());
    CHECK(ReadFile(root / "model_Wood.amat") == firstAmat);
    CHECK(ReadFile(root / "model_Wood.amat.aast") == firstSidecar);

    fs::remove_all(root);
}
