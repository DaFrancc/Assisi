/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <expected>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <Assisi/Core/AssetDatabase.hpp>
#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetSidecar.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/LooseFileProvider.hpp>

using namespace Assisi::Core;

namespace
{
namespace fs = std::filesystem;

void WriteFile(const fs::path &path, std::string_view contents)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::string ReadFile(const fs::path &path)
{
    std::ifstream     stream(path, std::ios::binary);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// A clean, populated asset tree rooted at a fresh temp directory. Returns the
// root. Two payload files, one already carrying a hand-written sidecar.
fs::path MakeTree()
{
    const fs::path root = fs::temp_directory_path() / "assisi_assetdb_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    WriteFile(root / "textures" / "crate.png", "PNG-BYTES");
    WriteFile(root / "materials" / "checker.amat", "{\"type\":\"MaterialData\"}");
    return root;
}
} // namespace

TEST_CASE("AssetDatabase generates missing sidecars and builds the map")
{
    const fs::path root = MakeTree();
    REQUIRE(AssetSystem::SetRoot(root).has_value());

    AssetDatabase db;
    auto          count = db.Rebuild();
    REQUIRE(count.has_value());
    CHECK(*count == 2); // two payload files registered

    // Count() includes the four reserved built-ins.
    CHECK(db.Count() == 2 + BuiltinAssets().size());

    // A sidecar now sits next to each payload file.
    CHECK(fs::exists(root / "textures" / "crate.png.aast"));
    CHECK(fs::exists(root / "materials" / "checker.amat.aast"));

    // Forward and reverse lookups agree.
    auto crateId = db.IdFor("textures/crate.png");
    REQUIRE(crateId.has_value());
    CHECK(db.PathFor(*crateId) == "textures/crate.png");
    CHECK_FALSE(crateId->IsReserved());

    // Built-ins resolve to their prim paths.
    CHECK(db.PathFor(BuiltinAssetId::Cube) == "prim://cube");
}

TEST_CASE("AssetDatabase never sidecars a sidecar")
{
    const fs::path root = MakeTree();
    REQUIRE(AssetSystem::SetRoot(root).has_value());

    AssetDatabase db;
    REQUIRE(db.Rebuild().has_value());

    // The generated .aast files must not themselves get ".aast.aast" — else the
    // scan would never terminate.
    CHECK_FALSE(fs::exists(root / "textures" / "crate.png.aast.aast"));
    CHECK_FALSE(db.IdFor("textures/crate.png.aast").has_value());
}

TEST_CASE("AssetDatabase leaves an existing sidecar untouched (reconcile-not-clobber)")
{
    const fs::path root = MakeTree();

    // Pre-write a sidecar with a known id for crate.png.
    const AssetId knownId = *AssetId::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const fs::path sidecar = root / "textures" / "crate.png.aast";
    const std::string original = SerializeSidecar(AssetSidecar{.guid = knownId});
    WriteFile(sidecar, original);

    REQUIRE(AssetSystem::SetRoot(root).has_value());
    AssetDatabase db;
    REQUIRE(db.Rebuild().has_value());

    // The id is honored, and the file's bytes are exactly as written.
    CHECK(db.IdFor("textures/crate.png") == knownId);
    CHECK(db.PathFor(knownId) == "textures/crate.png");
    CHECK(ReadFile(sidecar) == original);
}

TEST_CASE("AssetDatabase skips a malformed sidecar without clobbering it")
{
    const fs::path root = MakeTree();

    const fs::path    sidecar = root / "textures" / "crate.png.aast";
    const std::string garbage = "this is not json";
    WriteFile(sidecar, garbage);

    REQUIRE(AssetSystem::SetRoot(root).has_value());
    AssetDatabase db;
    auto          count = db.Rebuild();
    REQUIRE(count.has_value());

    // crate.png is skipped (unparseable id), checker.amat still registers.
    CHECK(*count == 1);
    CHECK_FALSE(db.IdFor("textures/crate.png").has_value());
    CHECK(db.IdFor("materials/checker.amat").has_value());
    // The malformed file is left exactly as-is.
    CHECK(ReadFile(sidecar) == garbage);
}

TEST_CASE("AssetDatabase::Rebuild is idempotent")
{
    const fs::path root = MakeTree();
    REQUIRE(AssetSystem::SetRoot(root).has_value());

    AssetDatabase db;
    REQUIRE(db.Rebuild().has_value());
    const AssetId firstId = *db.IdFor("textures/crate.png");
    const std::string firstSidecar = ReadFile(root / "textures" / "crate.png.aast");

    // A second scan mints nothing new and preserves ids.
    auto second = db.Rebuild();
    REQUIRE(second.has_value());
    CHECK(*second == 2);
    CHECK(db.IdFor("textures/crate.png") == firstId);
    CHECK(ReadFile(root / "textures" / "crate.png.aast") == firstSidecar);
}

TEST_CASE("AssetSidecar round-trips a composite manifest")
{
    const AssetId meshId = *AssetId::Parse("11111111-2222-4333-8444-555555555555");
    const AssetId matA   = *AssetId::Parse("aaaaaaaa-0000-4000-8000-000000000001");
    const AssetId matB   = *AssetId::Parse("aaaaaaaa-0000-4000-8000-000000000002");

    AssetSidecar sidecar{.guid = meshId};
    sidecar.subAssets.push_back(AssetSubAsset{.slot = 0, .material = matA});
    sidecar.subAssets.push_back(AssetSubAsset{.slot = 1, .material = matB});

    const std::expected<AssetSidecar, AssetSidecarError> parsed = DeserializeSidecar(SerializeSidecar(sidecar));
    REQUIRE(parsed.has_value());
    CHECK(parsed->guid == meshId);
    REQUIRE(parsed->subAssets.size() == 2);
    CHECK(parsed->subAssets[0].slot == 0);
    CHECK(parsed->subAssets[0].material == matA);
    CHECK(parsed->subAssets[1].slot == 1);
    CHECK(parsed->subAssets[1].material == matB);

    // A leaf sidecar (no manifest) stays that way, and is byte-identical to S1.
    const std::expected<AssetSidecar, AssetSidecarError> leaf =
        DeserializeSidecar(SerializeSidecar(AssetSidecar{.guid = meshId}));
    REQUIRE(leaf.has_value());
    CHECK(leaf->subAssets.empty());
}

TEST_CASE("AssetDatabase reads a manifest from a sidecar and answers SlotMaterial")
{
    const fs::path root = MakeTree();

    // Give checker.amat a known manifest via a hand-written glTF-style sidecar on
    // crate.png (any file can carry one; the DB does not care about the type).
    const AssetId meshId = *AssetId::Parse("11111111-2222-4333-8444-555555555555");
    const AssetId matId  = *AssetId::Parse("aaaaaaaa-0000-4000-8000-000000000009");
    AssetSidecar  sidecar{.guid = meshId};
    sidecar.subAssets.push_back(AssetSubAsset{.slot = 2, .material = matId});
    WriteFile(root / "textures" / "crate.png.aast", SerializeSidecar(sidecar));

    REQUIRE(AssetSystem::SetRoot(root).has_value());
    AssetDatabase db;
    REQUIRE(db.Rebuild().has_value());

    CHECK(db.HasManifest(meshId));
    CHECK(db.SlotMaterial(meshId, 2) == matId);
    // A gap slot and an out-of-range slot resolve to nil, not a crash.
    CHECK(db.SlotMaterial(meshId, 0).IsNil());
    CHECK(db.SlotMaterial(meshId, 7).IsNil());
    // A leaf asset (checker.amat) has no manifest.
    const AssetId checkerId = *db.IdFor("materials/checker.amat");
    CHECK_FALSE(db.HasManifest(checkerId));

    // Assets() lists the file assets (both payloads), never the built-ins.
    const std::vector<std::pair<AssetId, std::string>> assets = db.Assets();
    CHECK(assets.size() == 2);
    for (const auto &[id, path] : assets)
        CHECK_FALSE(id.IsReserved());
}

TEST_CASE("LooseFileProvider reads bytes by id and rejects unknown ids")
{
    const fs::path root = MakeTree();
    REQUIRE(AssetSystem::SetRoot(root).has_value());

    AssetDatabase db;
    REQUIRE(db.Rebuild().has_value());
    LooseFileProvider provider(db);

    const AssetId crateId = *db.IdFor("textures/crate.png");
    auto          bytes   = provider.Open(crateId);
    REQUIRE(bytes.has_value());
    const std::string text(reinterpret_cast<const char *>(bytes->data()), bytes->size());
    CHECK(text == "PNG-BYTES");

    // A reserved built-in is not a byte payload here.
    auto builtin = provider.Open(BuiltinAssetId::Cube);
    REQUIRE_FALSE(builtin.has_value());
    CHECK(builtin.error() == AssetError::UnknownAssetId);

    // A minted-but-unregistered id is unknown.
    auto missing = provider.Open(MintAssetId());
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == AssetError::UnknownAssetId);
}
