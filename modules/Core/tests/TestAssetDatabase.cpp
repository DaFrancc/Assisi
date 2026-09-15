/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
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
    std::ifstream stream(path, std::ios::binary);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// A clean asset tree rooted at a fresh temp directory: two payload files and no
// sidecars. Returns the root. Cases that want a sidecar write their own.
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
    auto count = db.Rebuild();
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
    const std::string original = SerializeSidecar(AssetSidecar::Leaf(knownId));
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

    const fs::path sidecar = root / "textures" / "crate.png.aast";
    const std::string garbage = "this is not json";
    WriteFile(sidecar, garbage);

    REQUIRE(AssetSystem::SetRoot(root).has_value());
    AssetDatabase db;
    auto count = db.Rebuild();
    REQUIRE(count.has_value());

    // crate.png is skipped (unparseable id), checker.amat still registers.
    CHECK(*count == 1);
    CHECK_FALSE(db.IdFor("textures/crate.png").has_value());
    CHECK(db.IdFor("materials/checker.amat").has_value());
    // The malformed file is left exactly as-is.
    CHECK(ReadFile(sidecar) == garbage);
}

TEST_CASE("AssetDatabase treats a zero-byte sidecar as malformed, not unreadable")
{
    // A comment sweep suspected that ReadWholeFile (AssetDatabase.cpp:30) rejects an
    // empty file: `buffer << stream.rdbuf()` does set failbit when it inserts no
    // characters, so a zero-byte sidecar looked like an I/O error. It sets that
    // bit on `buffer` — the ostringstream being written to — and the guard at
    // :39 tests `stream`, the ifstream, whose state the insertion never touches.
    // The empty read therefore succeeds with an empty string and the sidecar
    // falls through to DeserializeSidecar, which is where it belongs.
    //
    // Pinned rather than dropped: the behaviour is right, and it is right by an
    // accident of which object carries the flag, so a tidy-up that moved the
    // check onto `buffer` would silently turn a malformed sidecar into an
    // unreadable one — same skip, wrong diagnosis, and a truncated write is a
    // real way to produce this file.
    const fs::path root = MakeTree();

    const fs::path sidecar = root / "textures" / "crate.png.aast";
    WriteFile(sidecar, "");
    REQUIRE(fs::file_size(sidecar) == 0u);

    REQUIRE(AssetSystem::SetRoot(root).has_value());
    AssetDatabase db;
    auto count = db.Rebuild();
    REQUIRE(count.has_value());

    // Same outcome as the malformed-sidecar case above: crate.png is skipped and
    // its file is left untouched, rather than being re-minted over.
    CHECK(*count == 1);
    CHECK_FALSE(db.IdFor("textures/crate.png").has_value());
    CHECK(db.IdFor("materials/checker.amat").has_value());
    CHECK(fs::file_size(sidecar) == 0u);
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

    AssetSidecar sidecar = AssetSidecar::Leaf(meshId);
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

    // A leaf sidecar stays a leaf: no manifest, no source hash.
    const std::expected<AssetSidecar, AssetSidecarError> leaf =
        DeserializeSidecar(SerializeSidecar(AssetSidecar::Leaf(meshId)));
    REQUIRE(leaf.has_value());
    CHECK(leaf->subAssets.empty());
    CHECK_FALSE(leaf->sourceHash.has_value());
}

TEST_CASE("AssetSidecar round-trips a source hash")
{
    const AssetId meshId = *AssetId::Parse("11111111-2222-4333-8444-555555555555");

    AssetSidecar sidecar = AssetSidecar::Leaf(meshId);
    sidecar.subAssets.push_back(AssetSubAsset{.slot = 0, .material = meshId});
    sidecar.sourceHash = 0xdeadbeefcafef00dULL;

    const std::expected<AssetSidecar, AssetSidecarError> parsed = DeserializeSidecar(SerializeSidecar(sidecar));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->sourceHash.has_value());
    CHECK(*parsed->sourceHash == 0xdeadbeefcafef00dULL);
}

TEST_CASE("AssetDatabase reads a manifest from a sidecar and answers SlotMaterial")
{
    const fs::path root = MakeTree();

    // Give crate.png a hand-written glTF-style sidecar carrying a manifest (any
    // file can carry one; the DB does not care about the type).
    const AssetId meshId = *AssetId::Parse("11111111-2222-4333-8444-555555555555");
    const AssetId matId  = *AssetId::Parse("aaaaaaaa-0000-4000-8000-000000000009");
    AssetSidecar sidecar = AssetSidecar::Leaf(meshId);
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

// --- Malformed-sidecar robustness -------------------------------------------
// The contract is "skip a malformed sidecar with a warning, never crash." These
// exercise wrong-typed JSON fields (the naive `value(key, default)` spelling
// throws on those) and a hostile manifest slot (which sizes a vector).

TEST_CASE("DeserializeSidecar returns an error, never throws, on a wrong-typed 'type'")
{
    // "type" present but a number: `value("type", std::string{})` would throw
    // json::type_error rather than substituting the default.
    std::expected<AssetSidecar, AssetSidecarError> result;
    CHECK_NOTHROW(result = DeserializeSidecar(R"({"version":1,"type":123})"));
    CHECK_FALSE(result.has_value());
}

TEST_CASE("DeserializeSidecar returns an error, never throws, on a wrong-typed 'guid'")
{
    std::expected<AssetSidecar, AssetSidecarError> result;
    CHECK_NOTHROW(result = DeserializeSidecar(R"({"version":1,"type":"AssetSidecar","guid":42})"));
    CHECK_FALSE(result.has_value());
}

TEST_CASE("DeserializeSidecar skips (does not throw on) a wrong-typed manifest 'material'")
{
    const std::string json = R"({"version":1,"type":"AssetSidecar",)"
                             R"("guid":"11111111-2222-4333-8444-555555555555",)"
                             R"("subAssets":[{"slot":0,"material":99}]})";
    std::expected<AssetSidecar, AssetSidecarError> result;
    CHECK_NOTHROW(result = DeserializeSidecar(json));
    REQUIRE(result.has_value()); // identity valid; the bad entry is dropped
    CHECK(result->subAssets.empty());
}

TEST_CASE("DeserializeSidecar sanitizes a negative manifest slot (no ~0u wrap)")
{
    const std::string json = R"({"version":1,"type":"AssetSidecar",)"
                             R"("guid":"11111111-2222-4333-8444-555555555555",)"
                             R"("subAssets":[{"slot":-1,"material":"aaaaaaaa-0000-4000-8000-000000000001"}]})";
    std::expected<AssetSidecar, AssetSidecarError> parsed;
    CHECK_NOTHROW(parsed = DeserializeSidecar(json));
    REQUIRE(parsed.has_value());
    // Rejected, or a sane slot — never ~0u, which Rebuild would size a vector to.
    if (!parsed->subAssets.empty())
        CHECK(parsed->subAssets[0].slot < (1u << 20));
}

TEST_CASE("Rebuild survives a sidecar with an out-of-range manifest slot")
{
    // A huge but syntactically valid unsigned slot. Unrejected, it would size the
    // slot vector to ~4 billion entries (a multi-GB reservation / OOM risk); the
    // slot cap must drop the entry instead.
    const fs::path root = MakeTree();
    const std::string json = R"({"version":1,"type":"AssetSidecar",)"
                             R"("guid":"11111111-2222-4333-8444-555555555555",)"
                             R"("subAssets":[{"slot":4294967294,"material":"aaaaaaaa-0000-4000-8000-000000000001"}]})";
    WriteFile(root / "materials" / "checker.amat.aast", json);

    REQUIRE(AssetSystem::SetRoot(root).has_value());
    AssetDatabase db;
    CHECK_NOTHROW((void)db.Rebuild()); // nodiscard: this case only asserts it does not throw
    CHECK(db.IdFor("materials/checker.amat").has_value());
    CHECK_FALSE(db.HasManifest(*AssetId::Parse("11111111-2222-4333-8444-555555555555")));
}

TEST_CASE("Rebuild survives a sidecar with a wrapping (negative) manifest slot")
{
    // A naive read of slot -1 yields 0xFFFFFFFF, and `resize(slot + 1)` in uint32
    // wraps that to resize(0) before writing slots[0xFFFFFFFF] — an out-of-bounds
    // heap write. The entry must be dropped at deserialize instead.
    const fs::path root = MakeTree();
    const std::string json = R"({"version":1,"type":"AssetSidecar",)"
                             R"("guid":"11111111-2222-4333-8444-555555555555",)"
                             R"("subAssets":[{"slot":-1,"material":"aaaaaaaa-0000-4000-8000-000000000001"}]})";
    WriteFile(root / "materials" / "checker.amat.aast", json);

    REQUIRE(AssetSystem::SetRoot(root).has_value());
    AssetDatabase db;
    CHECK_NOTHROW((void)db.Rebuild()); // nodiscard: this case only asserts it does not throw
    // checker.amat still registers; the bogus manifest slot is dropped.
    CHECK(db.IdFor("materials/checker.amat").has_value());
    CHECK_FALSE(db.HasManifest(*AssetId::Parse("11111111-2222-4333-8444-555555555555")));
}

TEST_CASE("Rebuild mints no sidecar for an ignored file, nor for the ignore list itself")
{
    const fs::path root = MakeTree();
    WriteFile(root / "models" / "source.zip", "PK");
    WriteFile(root / ".assisiignore", "*.zip\n");

    REQUIRE(AssetSystem::SetRoot(root).has_value());
    AssetDatabase db;
    const std::expected<std::size_t, AssetError> count = db.Rebuild();
    REQUIRE(count.has_value());

    // Only the two payload files from MakeTree: the archive and the list itself
    // are both out.
    CHECK(*count == 2);
    CHECK_FALSE(db.IdFor("models/source.zip").has_value());
    CHECK_FALSE(db.IdFor(".assisiignore").has_value());

    // Ignoring is not deleting: the files stay, but nothing was written beside
    // them, so neither has become addressable content.
    CHECK(fs::exists(root / "models" / "source.zip"));
    CHECK_FALSE(fs::exists(root / "models" / "source.zip.aast"));
    CHECK_FALSE(fs::exists(root / ".assisiignore.aast"));
}

TEST_CASE("Rebuild honours a negation that re-includes one file inside an ignored directory")
{
    const fs::path root = MakeTree();
    WriteFile(root / "editor" / "entity_icon.png", "PNG-BYTES");
    WriteFile(root / "editor" / "loading" / "Spinner.webp", "WEBP-BYTES");
    WriteFile(root / ".assisiignore", "/editor/\n"
                                      "!/editor/loading/Spinner.webp\n");

    REQUIRE(AssetSystem::SetRoot(root).has_value());
    AssetDatabase db;
    const std::expected<std::size_t, AssetError> count = db.Rebuild();
    REQUIRE(count.has_value());

    // The two payloads from MakeTree, plus the one file the negation takes back.
    // The walk must not prune the excluded directory, or it would never reach it.
    CHECK(*count == 3);
    CHECK(db.IdFor("editor/loading/Spinner.webp").has_value());
    CHECK(fs::exists(root / "editor" / "loading" / "Spinner.webp.aast"));
    CHECK_FALSE(db.IdFor("editor/entity_icon.png").has_value());
    CHECK_FALSE(fs::exists(root / "editor" / "entity_icon.png.aast"));
}

TEST_CASE("LooseFileProvider reads bytes by id and rejects unknown ids")
{
    const fs::path root = MakeTree();
    REQUIRE(AssetSystem::SetRoot(root).has_value());

    AssetDatabase db;
    REQUIRE(db.Rebuild().has_value());
    LooseFileProvider provider(db);

    const AssetId crateId = *db.IdFor("textures/crate.png");
    auto bytes   = provider.Open(crateId);
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

TEST_CASE("A duplicate asset id is re-minted rather than left unaddressable")
{
    const fs::path root = MakeTree();

    // Two distinct assets carrying the SAME sidecar id — what copying an asset
    // together with its sidecar produces.
    const AssetId shared = *AssetId::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    WriteFile(root / "textures" / "copy_a.png", "A");
    WriteFile(root / "textures" / "copy_b.png", "B");
    WriteFile(root / "textures" / "copy_a.png.aast", SerializeSidecar(AssetSidecar::Leaf(shared)));
    WriteFile(root / "textures" / "copy_b.png.aast", SerializeSidecar(AssetSidecar::Leaf(shared)));

    REQUIRE(AssetSystem::SetRoot(root).has_value());
    AssetDatabase db;
    REQUIRE(db.Rebuild().has_value());

    // First writer keeps the shared id; the loser is re-minted, NOT dropped —
    // dropping is permanent, since its sidecar parses fine and so the
    // mint-on-missing path never fires for it on any later Rebuild.
    const std::optional<AssetId> idA = db.IdFor("textures/copy_a.png");
    const std::optional<AssetId> idB = db.IdFor("textures/copy_b.png");
    REQUIRE(idA.has_value());
    REQUIRE(idB.has_value());
    CHECK(*idA != *idB);
    CHECK((*idA == shared || *idB == shared)); // exactly one kept the original

    // The re-mint was persisted, so it is stable across a rebuild.
    AssetDatabase second;
    REQUIRE(second.Rebuild().has_value());
    CHECK(second.IdFor("textures/copy_a.png") == idA);
    CHECK(second.IdFor("textures/copy_b.png") == idB);
}

