/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <expected>
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

// Overwrites model.gltf with @p text — stands in for a DCC re-export.
void OverwriteGltf(const fs::path &root, std::string_view text)
{
    std::ofstream gltf(root / "model.gltf", std::ios::binary | std::ios::trunc);
    gltf.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// Resolve a material GUID to its `.amat` virtual path by scanning the root for
// the sidecar carrying it — the reconciler's AssetDatabase stand-in for tests.
std::string ResolveMaterialPathIn(const fs::path &root, const AssetId &id)
{
    for (const fs::directory_entry &entry : fs::recursive_directory_iterator(root))
    {
        if (entry.path().extension() != ".aast" || entry.path().stem().extension() != ".amat")
            continue;
        const std::expected<AssetSidecar, Assisi::Core::AssetSidecarError> side =
            DeserializeSidecar(ReadFile(entry.path()));
        if (side && side->guid == id)
            return fs::relative(entry.path().parent_path() / entry.path().stem(), root).generic_string();
    }
    return {};
}

// A material glTF with a second, texture-less "Metal" material on a second
// primitive — reconciling from kMaterialGltf to this appends slot 1 (Wood is
// byte-identical), the additive-safe case.
constexpr std::string_view kTwoMaterialGltf = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [
    { "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 },
    { "attributes": { "POSITION": 0 }, "indices": 1, "material": 1 }
  ] } ],
  "materials": [
    { "name": "Wood", "pbrMetallicRoughness": {
        "baseColorFactor": [0.8, 0.6, 0.4, 1.0], "baseColorTexture": { "index": 0 } } },
    { "name": "Metal", "pbrMetallicRoughness": { "baseColorFactor": [0.2, 0.2, 0.2, 1.0] } }
  ],
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

// kMaterialGltf with the base-color factor changed — the material differs, so a
// reconcile can't prove it safe (ConflictStale).
constexpr std::string_view kMaterialGltfChangedFactor = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 } ] } ],
  "materials": [ { "name": "Wood", "pbrMetallicRoughness": {
      "baseColorFactor": [0.1, 0.2, 0.3, 1.0], "baseColorTexture": { "index": 0 } } } ],
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

// kMaterialGltf with only a non-material change (the accessor bounds) — same
// materials, different source bytes: GeometryOnly.
constexpr std::string_view kMaterialGltfChangedGeometry = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 } ] } ],
  "materials": [ { "name": "Wood", "pbrMetallicRoughness": {
      "baseColorFactor": [0.8, 0.6, 0.4, 1.0], "baseColorTexture": { "index": 0 } } } ],
  "textures": [ { "source": 0 } ],
  "images": [ { "uri": "wood.png" } ],
  "buffers": [ { "uri": "triangle.bin", "byteLength": 42 } ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0,  "byteLength": 36, "target": 34962 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 6,  "target": 34963 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
      "min": [0.0, 0.0, 0.0], "max": [2.0, 2.0, 0.0] },
    { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
  ]
})";

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
        const std::string text = SerializeSidecar(AssetSidecar::Leaf(gltfId));
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
    CHECK(gltfSidecar->sourceHash.has_value()); // stamped for S4 stale detection
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

// --- Reconcile (S4/D5) -----------------------------------------------------

using Assisi::Geometry::ReconcileGltfMaterials;
using Assisi::Geometry::ReconcileOutcome;
using Assisi::Geometry::ReconcileResult;

namespace
{
// The two resolvers ReconcileGltfMaterials needs, closed over a test root.
auto MakeTextureResolver(AssetId woodId)
{
    return [woodId](std::string_view path) -> AssetId { return path == "wood.png" ? woodId : AssetId{}; };
}
} // namespace

TEST_CASE("ReconcileGltfMaterials: unchanged source is up to date")
{
    const AssetId  gltfId = MintAssetId();
    const AssetId  woodId = MintAssetId();
    const fs::path root   = WriteMaterialAssets(gltfId);
    const auto     resolveTex = MakeTextureResolver(woodId);
    const auto     resolveMat = [&root](const AssetId &id) { return ResolveMaterialPathIn(root, id); };
    REQUIRE(ExplodeGltfMaterials("model.gltf", resolveTex).has_value());

    const ReconcileResult result = ReconcileGltfMaterials("model.gltf", resolveTex, resolveMat);
    CHECK(result.outcome == ReconcileOutcome::UpToDate);
    CHECK_FALSE(result.changedDisk);

    fs::remove_all(root);
}

TEST_CASE("ReconcileGltfMaterials: a pre-S4 manifest (no hash) is stamped")
{
    const AssetId  gltfId = MintAssetId();
    const AssetId  woodId = MintAssetId();
    const fs::path root   = WriteMaterialAssets(gltfId);
    const auto     resolveTex = MakeTextureResolver(woodId);
    const auto     resolveMat = [&root](const AssetId &id) { return ResolveMaterialPathIn(root, id); };
    REQUIRE(ExplodeGltfMaterials("model.gltf", resolveTex).has_value());

    // Strip the stamped hash to simulate an S3-era sidecar.
    std::expected<AssetSidecar, Assisi::Core::AssetSidecarError> side =
        DeserializeSidecar(ReadFile(root / "model.gltf.aast"));
    REQUIRE(side.has_value());
    side->sourceHash.reset();
    {
        std::ofstream out(root / "model.gltf.aast", std::ios::binary | std::ios::trunc);
        const std::string text = SerializeSidecar(*side);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    const ReconcileResult result = ReconcileGltfMaterials("model.gltf", resolveTex, resolveMat);
    CHECK(result.outcome == ReconcileOutcome::Stamped);
    // The sidecar now carries a hash; a follow-up reconcile is up to date.
    const std::expected<AssetSidecar, Assisi::Core::AssetSidecarError> restamped =
        DeserializeSidecar(ReadFile(root / "model.gltf.aast"));
    REQUIRE(restamped.has_value());
    CHECK(restamped->sourceHash.has_value());
    CHECK(ReconcileGltfMaterials("model.gltf", resolveTex, resolveMat).outcome == ReconcileOutcome::UpToDate);

    fs::remove_all(root);
}

TEST_CASE("ReconcileGltfMaterials: a changed material is left stale, untouched")
{
    const AssetId  gltfId = MintAssetId();
    const AssetId  woodId = MintAssetId();
    const fs::path root   = WriteMaterialAssets(gltfId);
    const auto     resolveTex = MakeTextureResolver(woodId);
    const auto     resolveMat = [&root](const AssetId &id) { return ResolveMaterialPathIn(root, id); };
    REQUIRE(ExplodeGltfMaterials("model.gltf", resolveTex).has_value());
    const std::string amatBefore = ReadFile(root / "model_Wood.amat");

    OverwriteGltf(root, kMaterialGltfChangedFactor);
    const ReconcileResult result = ReconcileGltfMaterials("model.gltf", resolveTex, resolveMat);
    CHECK(result.outcome == ReconcileOutcome::ConflictStale);
    CHECK_FALSE(result.changedDisk);
    // The authored material is not clobbered, and the mismatch keeps being seen.
    CHECK(ReadFile(root / "model_Wood.amat") == amatBefore);
    CHECK(ReconcileGltfMaterials("model.gltf", resolveTex, resolveMat).outcome == ReconcileOutcome::ConflictStale);

    fs::remove_all(root);
}

TEST_CASE("ReconcileGltfMaterials: a non-material change refreshes the hash only")
{
    const AssetId  gltfId = MintAssetId();
    const AssetId  woodId = MintAssetId();
    const fs::path root   = WriteMaterialAssets(gltfId);
    const auto     resolveTex = MakeTextureResolver(woodId);
    const auto     resolveMat = [&root](const AssetId &id) { return ResolveMaterialPathIn(root, id); };
    REQUIRE(ExplodeGltfMaterials("model.gltf", resolveTex).has_value());
    const std::string amatBefore = ReadFile(root / "model_Wood.amat");

    OverwriteGltf(root, kMaterialGltfChangedGeometry);
    const ReconcileResult result = ReconcileGltfMaterials("model.gltf", resolveTex, resolveMat);
    CHECK(result.outcome == ReconcileOutcome::GeometryOnly);
    CHECK(result.changedDisk);
    CHECK(ReadFile(root / "model_Wood.amat") == amatBefore); // material untouched
    // Hash refreshed, so the next reconcile is up to date.
    CHECK(ReconcileGltfMaterials("model.gltf", resolveTex, resolveMat).outcome == ReconcileOutcome::UpToDate);

    fs::remove_all(root);
}

TEST_CASE("ReconcileGltfMaterials: a new slot is materialized (additive)")
{
    const AssetId  gltfId = MintAssetId();
    const AssetId  woodId = MintAssetId();
    const fs::path root   = WriteMaterialAssets(gltfId);
    const auto     resolveTex = MakeTextureResolver(woodId);
    const auto     resolveMat = [&root](const AssetId &id) { return ResolveMaterialPathIn(root, id); };
    REQUIRE(ExplodeGltfMaterials("model.gltf", resolveTex).has_value());
    const std::string woodBefore = ReadFile(root / "model_Wood.amat");

    OverwriteGltf(root, kTwoMaterialGltf);
    const ReconcileResult result = ReconcileGltfMaterials("model.gltf", resolveTex, resolveMat);
    CHECK(result.outcome == ReconcileOutcome::AdditiveSlots);
    CHECK(result.addedSlots == 1);
    CHECK(result.changedDisk);
    // The existing slot is untouched; the new slot gets its own default .amat.
    CHECK(ReadFile(root / "model_Wood.amat") == woodBefore);
    CHECK(fs::exists(root / "model_Metal_1.amat"));

    const std::expected<AssetSidecar, Assisi::Core::AssetSidecarError> side =
        DeserializeSidecar(ReadFile(root / "model.gltf.aast"));
    REQUIRE(side.has_value());
    CHECK(side->subAssets.size() == 2); // both slots now bound
    CHECK(ReconcileGltfMaterials("model.gltf", resolveTex, resolveMat).outcome == ReconcileOutcome::UpToDate);

    fs::remove_all(root);
}

// --- Prompt-driven conflict resolution (S4 second half / D5) ----------------

using Assisi::Geometry::AcceptGltfSource;
using Assisi::Geometry::DiffGltfMaterials;
using Assisi::Geometry::MaterialDiff;
using Assisi::Geometry::RegenerateGltfMaterials;
using Assisi::Geometry::SlotChange;

// The GUID recorded in an `.amat`'s sidecar — to assert regenerate preserves it.
AssetId AmatIdIn(const fs::path &amatSidecar)
{
    const std::expected<AssetSidecar, Assisi::Core::AssetSidecarError> side = DeserializeSidecar(ReadFile(amatSidecar));
    return side ? side->guid : AssetId{};
}

TEST_CASE("DiffGltfMaterials: a changed factor is reported as a per-slot conflict")
{
    const AssetId  gltfId = MintAssetId();
    const AssetId  woodId = MintAssetId();
    const fs::path root   = WriteMaterialAssets(gltfId);
    const auto     resolveTex = MakeTextureResolver(woodId);
    const auto     resolveMat = [&root](const AssetId &id) { return ResolveMaterialPathIn(root, id); };
    REQUIRE(ExplodeGltfMaterials("model.gltf", resolveTex).has_value());

    OverwriteGltf(root, kMaterialGltfChangedFactor);
    const MaterialDiff diff = DiffGltfMaterials("model.gltf", resolveTex, resolveMat);
    REQUIRE(diff.valid);
    REQUIRE(diff.slots.size() == 1);
    CHECK(diff.slots[0].slot == 0);
    CHECK(diff.slots[0].change == SlotChange::Changed);
    CHECK(diff.slots[0].name == "Wood");
    CHECK(diff.HasConflict());

    fs::remove_all(root);
}

TEST_CASE("DiffGltfMaterials: an appended slot is Added, a dropped slot is Removed")
{
    const AssetId  gltfId = MintAssetId();
    const AssetId  woodId = MintAssetId();
    const fs::path root   = WriteMaterialAssets(gltfId);
    const auto     resolveTex = MakeTextureResolver(woodId);
    const auto     resolveMat = [&root](const AssetId &id) { return ResolveMaterialPathIn(root, id); };

    // Explode the two-material version, then drop back to one material: slot 0
    // survives unchanged, slot 1 is removed.
    OverwriteGltf(root, kTwoMaterialGltf);
    REQUIRE(ExplodeGltfMaterials("model.gltf", resolveTex).has_value());
    OverwriteGltf(root, kMaterialGltf);

    const MaterialDiff removedDiff = DiffGltfMaterials("model.gltf", resolveTex, resolveMat);
    REQUIRE(removedDiff.valid);
    REQUIRE(removedDiff.slots.size() == 2);
    CHECK(removedDiff.slots[0].change == SlotChange::Unchanged);
    CHECK(removedDiff.slots[1].change == SlotChange::Removed);
    CHECK(removedDiff.HasConflict());

    // The reverse (one material exploded, source grows to two) reports slot 1
    // Added — the additive-safe case, no conflict.
    const fs::path  root2   = WriteMaterialAssets(MintAssetId());
    const auto      resMat2 = [&root2](const AssetId &id) { return ResolveMaterialPathIn(root2, id); };
    REQUIRE(ExplodeGltfMaterials("model.gltf", resolveTex).has_value());
    OverwriteGltf(root2, kTwoMaterialGltf);
    const MaterialDiff addedDiff = DiffGltfMaterials("model.gltf", resolveTex, resMat2);
    REQUIRE(addedDiff.valid);
    REQUIRE(addedDiff.slots.size() == 2);
    CHECK(addedDiff.slots[0].change == SlotChange::Unchanged);
    CHECK(addedDiff.slots[1].change == SlotChange::Added);
    CHECK_FALSE(addedDiff.HasConflict());

    fs::remove_all(root);
    fs::remove_all(root2);
}

TEST_CASE("RegenerateGltfMaterials: overwrites the material from source, keeping its GUID")
{
    const AssetId  gltfId = MintAssetId();
    const AssetId  woodId = MintAssetId();
    const fs::path root   = WriteMaterialAssets(gltfId);
    const auto     resolveTex = MakeTextureResolver(woodId);
    const auto     resolveMat = [&root](const AssetId &id) { return ResolveMaterialPathIn(root, id); };
    REQUIRE(ExplodeGltfMaterials("model.gltf", resolveTex).has_value());
    const AssetId woodBefore = AmatIdIn(root / "model_Wood.amat.aast");

    OverwriteGltf(root, kMaterialGltfChangedFactor);
    REQUIRE(ReconcileGltfMaterials("model.gltf", resolveTex, resolveMat).outcome == ReconcileOutcome::ConflictStale);

    const std::optional<std::size_t> slots = RegenerateGltfMaterials("model.gltf", resolveTex, resolveMat);
    REQUIRE(slots.has_value());
    CHECK(*slots == 1);

    // The body now carries the new source factor, and the GUID is preserved.
    const std::expected<Assisi::Geometry::MaterialData, Assisi::Geometry::MaterialFileError> material =
        DeserializeMaterial(ReadFile(root / "model_Wood.amat"));
    REQUIRE(material.has_value());
    CHECK(material->BaseColorFactor.x == doctest::Approx(0.1f));
    CHECK(AmatIdIn(root / "model_Wood.amat.aast") == woodBefore);

    // The conflict is cleared — a follow-up reconcile is up to date.
    CHECK(ReconcileGltfMaterials("model.gltf", resolveTex, resolveMat).outcome == ReconcileOutcome::UpToDate);

    fs::remove_all(root);
}

TEST_CASE("RegenerateGltfMaterials: a dropped slot leaves the manifest short, orphaned file kept")
{
    const AssetId  gltfId = MintAssetId();
    const AssetId  woodId = MintAssetId();
    const fs::path root   = WriteMaterialAssets(gltfId);
    const auto     resolveTex = MakeTextureResolver(woodId);
    const auto     resolveMat = [&root](const AssetId &id) { return ResolveMaterialPathIn(root, id); };

    OverwriteGltf(root, kTwoMaterialGltf);
    REQUIRE(ExplodeGltfMaterials("model.gltf", resolveTex).has_value());
    REQUIRE(fs::exists(root / "model_Metal.amat"));
    OverwriteGltf(root, kMaterialGltf); // slot 1 removed at source

    const std::optional<std::size_t> slots = RegenerateGltfMaterials("model.gltf", resolveTex, resolveMat);
    REQUIRE(slots.has_value());
    CHECK(*slots == 1); // only the surviving slot is bound now

    const std::expected<AssetSidecar, Assisi::Core::AssetSidecarError> side =
        DeserializeSidecar(ReadFile(root / "model.gltf.aast"));
    REQUIRE(side.has_value());
    CHECK(side->subAssets.size() == 1);
    // The dropped slot's file is orphaned, never deleted.
    CHECK(fs::exists(root / "model_Metal.amat"));
    CHECK(ReconcileGltfMaterials("model.gltf", resolveTex, resolveMat).outcome == ReconcileOutcome::UpToDate);

    fs::remove_all(root);
}

TEST_CASE("AcceptGltfSource: accepts the source without touching the materials")
{
    const AssetId  gltfId = MintAssetId();
    const AssetId  woodId = MintAssetId();
    const fs::path root   = WriteMaterialAssets(gltfId);
    const auto     resolveTex = MakeTextureResolver(woodId);
    const auto     resolveMat = [&root](const AssetId &id) { return ResolveMaterialPathIn(root, id); };
    REQUIRE(ExplodeGltfMaterials("model.gltf", resolveTex).has_value());
    const std::string amatBefore = ReadFile(root / "model_Wood.amat");

    OverwriteGltf(root, kMaterialGltfChangedFactor);
    REQUIRE(ReconcileGltfMaterials("model.gltf", resolveTex, resolveMat).outcome == ReconcileOutcome::ConflictStale);

    REQUIRE(AcceptGltfSource("model.gltf"));
    // The material is byte-identical (hand-edit kept), and the stale state clears.
    CHECK(ReadFile(root / "model_Wood.amat") == amatBefore);
    CHECK(ReconcileGltfMaterials("model.gltf", resolveTex, resolveMat).outcome == ReconcileOutcome::UpToDate);

    fs::remove_all(root);
}
