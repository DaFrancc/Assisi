/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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
