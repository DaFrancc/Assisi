/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestMaterialAuthoring.cpp
/// @brief The editor's `.amat` write path: saving an authored material, keeping
/// its GUID identity, and picking a free filename.
///
/// The load half is covered by TestMaterialFile; what is exercised here is
/// everything that touches the filesystem, because that is where an authoring
/// tool loses work — clobbering a sidecar, silently writing outside the asset
/// root, or dropping the fields a file did not happen to mention.

#include <doctest/doctest.h>

#include <expected>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Geometry/AssetImport.hpp>
#include <Assisi/Geometry/MaterialData.hpp>
#include <Assisi/Geometry/MaterialFile.hpp>

using Assisi::Core::AssetSystem;
using Assisi::Geometry::DeserializeMaterial;
using Assisi::Geometry::MaterialData;
using Assisi::Geometry::MaterialWriteError;
using Assisi::Geometry::SaveMaterial;
using Assisi::Geometry::UniqueMaterialPath;

namespace fs = std::filesystem;

namespace
{

std::string ReadFile(const fs::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void WriteFile(const fs::path &path, std::string_view text)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}

/// A fresh asset root, pointed at by AssetSystem, removed on scope exit. Each
/// case gets its own directory name so a leftover root from a failed run cannot
/// make a later case pass.
struct AssetRoot
{
    explicit AssetRoot(std::string_view name) : path(fs::temp_directory_path() / name)
    {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
        REQUIRE(AssetSystem::SetRoot(path).has_value());
    }
    ~AssetRoot()
    {
        AssetSystem::SetAuthoringRoot({}); // never leak the mirror into another case
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    AssetRoot(const AssetRoot &)            = delete;
    AssetRoot &operator=(const AssetRoot &) = delete;

    fs::path path;
};

/// Every field a `.amat` carries, at a distinct non-default value. Deliberately
/// exhaustive: a save path that dropped one would otherwise pass by writing the
/// default the reader would have supplied anyway.
MaterialData MakeFullMaterial()
{
    MaterialData m;
    m.BaseColorFactor      = {0.1f, 0.2f, 0.3f, 0.4f};
    m.MetallicFactor       = 0.25f;
    m.RoughnessFactor      = 0.6f;
    m.NormalScale          = 0.75f;
    m.OcclusionStrength    = 0.5f;
    m.EmissiveFactor       = {1.0f, 0.5f, 0.25f};
    m.BaseWeight           = 0.8f;
    m.SpecularWeight       = 0.7f;
    m.SpecularColor        = {0.9f, 0.8f, 0.6f};
    m.SpecularIor          = 1.33f;
    m.BaseDiffuseRoughness = 0.4f;
    m.BaseColorTexture         = *Assisi::Core::AssetId::Parse("aaaaaaaa-0000-4000-8000-000000000001");
    m.NormalTexture            = *Assisi::Core::AssetId::Parse("aaaaaaaa-0000-4000-8000-000000000002");
    m.MetallicRoughnessTexture = *Assisi::Core::AssetId::Parse("aaaaaaaa-0000-4000-8000-000000000003");
    m.OcclusionTexture         = *Assisi::Core::AssetId::Parse("aaaaaaaa-0000-4000-8000-000000000004");
    m.EmissiveTexture          = *Assisi::Core::AssetId::Parse("aaaaaaaa-0000-4000-8000-000000000005");
    return m;
}

/// Field-for-field equality over everything a `.amat` serializes. `Name` is
/// excluded because it deliberately never enters the file.
void CheckSameMaterial(const MaterialData &actual, const MaterialData &expected)
{
    CHECK(actual.BaseColorFactor.x == doctest::Approx(expected.BaseColorFactor.x));
    CHECK(actual.BaseColorFactor.y == doctest::Approx(expected.BaseColorFactor.y));
    CHECK(actual.BaseColorFactor.z == doctest::Approx(expected.BaseColorFactor.z));
    CHECK(actual.BaseColorFactor.w == doctest::Approx(expected.BaseColorFactor.w));
    CHECK(actual.MetallicFactor == doctest::Approx(expected.MetallicFactor));
    CHECK(actual.RoughnessFactor == doctest::Approx(expected.RoughnessFactor));
    CHECK(actual.NormalScale == doctest::Approx(expected.NormalScale));
    CHECK(actual.OcclusionStrength == doctest::Approx(expected.OcclusionStrength));
    CHECK(actual.EmissiveFactor.x == doctest::Approx(expected.EmissiveFactor.x));
    CHECK(actual.EmissiveFactor.y == doctest::Approx(expected.EmissiveFactor.y));
    CHECK(actual.EmissiveFactor.z == doctest::Approx(expected.EmissiveFactor.z));
    CHECK(actual.BaseWeight == doctest::Approx(expected.BaseWeight));
    CHECK(actual.SpecularWeight == doctest::Approx(expected.SpecularWeight));
    CHECK(actual.SpecularColor.x == doctest::Approx(expected.SpecularColor.x));
    CHECK(actual.SpecularColor.y == doctest::Approx(expected.SpecularColor.y));
    CHECK(actual.SpecularColor.z == doctest::Approx(expected.SpecularColor.z));
    CHECK(actual.SpecularIor == doctest::Approx(expected.SpecularIor));
    CHECK(actual.BaseDiffuseRoughness == doctest::Approx(expected.BaseDiffuseRoughness));
    CHECK(actual.BaseColorTexture == expected.BaseColorTexture);
    CHECK(actual.NormalTexture == expected.NormalTexture);
    CHECK(actual.MetallicRoughnessTexture == expected.MetallicRoughnessTexture);
    CHECK(actual.OcclusionTexture == expected.OcclusionTexture);
    CHECK(actual.EmissiveTexture == expected.EmissiveTexture);
}

} // namespace

TEST_CASE("SaveMaterial writes a .amat that reloads field-for-field")
{
    const AssetRoot root("assisi_matauthor_roundtrip");
    const MaterialData original = MakeFullMaterial();

    REQUIRE(SaveMaterial("materials/crate.amat", original).has_value());
    REQUIRE(fs::exists(root.path / "materials" / "crate.amat"));

    const std::expected<MaterialData, Assisi::Geometry::MaterialFileError> reloaded =
        DeserializeMaterial(ReadFile(root.path / "materials" / "crate.amat"));
    REQUIRE(reloaded.has_value());
    CheckSameMaterial(*reloaded, original);
}

TEST_CASE("SaveMaterial replaces the body and leaves the GUID sidecar untouched")
{
    const AssetRoot root("assisi_matauthor_sidecar");

    // A material already on disk with an id the reconcile pass minted for it.
    // Re-saving is an edit to *this* asset, so its identity must survive — every
    // level referencing it holds that GUID and nothing else.
    constexpr std::string_view kSidecar = R"({"guid":"bbbbbbbb-0000-4000-8000-00000000000f"})";
    WriteFile(root.path / "crate.amat", R"({"type":"MaterialData"})");
    WriteFile(root.path / "crate.amat.aast", kSidecar);

    MaterialData edited;
    edited.RoughnessFactor = 0.125f;
    REQUIRE(SaveMaterial("crate.amat", edited).has_value());

    CHECK(ReadFile(root.path / "crate.amat.aast") == kSidecar);

    const std::expected<MaterialData, Assisi::Geometry::MaterialFileError> reloaded =
        DeserializeMaterial(ReadFile(root.path / "crate.amat"));
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->RoughnessFactor == doctest::Approx(0.125f));
}

TEST_CASE("An old .amat opened and re-saved loses no fields")
{
    const AssetRoot root("assisi_matauthor_forwardcompat");

    // A file from before the OpenPBR fields existed: it mentions two factors and
    // nothing else. Loading fills the rest from their defaults, and re-saving
    // must write the whole schema back rather than the two keys it was handed.
    WriteFile(root.path / "old.amat", R"({
      "version": 1,
      "type": "MaterialData",
      "BaseColorFactor": [0.2, 0.4, 0.6, 1.0],
      "MetallicFactor": 0.3
    })");

    const std::expected<MaterialData, Assisi::Geometry::MaterialFileError> loaded =
        DeserializeMaterial(ReadFile(root.path / "old.amat"));
    REQUIRE(loaded.has_value());
    REQUIRE(SaveMaterial("old.amat", *loaded).has_value());

    const std::expected<MaterialData, Assisi::Geometry::MaterialFileError> resaved =
        DeserializeMaterial(ReadFile(root.path / "old.amat"));
    REQUIRE(resaved.has_value());

    // What the old file said is preserved...
    CHECK(resaved->BaseColorFactor.x == doctest::Approx(0.2f));
    CHECK(resaved->BaseColorFactor.z == doctest::Approx(0.6f));
    CHECK(resaved->MetallicFactor == doctest::Approx(0.3f));
    // ...and what it never mentioned is now written out explicitly, at the
    // defaults the loader supplied, rather than dropped.
    const std::string text = ReadFile(root.path / "old.amat");
    CHECK(text.find("SpecularIor") != std::string::npos);
    CHECK(text.find("BaseWeight") != std::string::npos);
    CHECK(text.find("BaseDiffuseRoughness") != std::string::npos);
    CHECK(resaved->SpecularIor == doctest::Approx(1.5f));
    CHECK(resaved->BaseWeight == doctest::Approx(1.f));
}

TEST_CASE("SaveMaterial refuses a path that escapes the asset root")
{
    const AssetRoot root("assisi_matauthor_escape");

    const std::expected<void, MaterialWriteError> result =
        SaveMaterial("../outside.amat", MaterialData{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MaterialWriteError::PathUnresolvable);
    CHECK_FALSE(fs::exists(root.path.parent_path() / "outside.amat"));
}

TEST_CASE("SaveMaterial mirrors the body into the authoring root")
{
    const AssetRoot root("assisi_matauthor_mirror");
    const fs::path authoring = fs::temp_directory_path() / "assisi_matauthor_mirror_src";
    std::error_code ec;
    fs::remove_all(authoring, ec);
    fs::create_directories(authoring);
    AssetSystem::SetAuthoringRoot(authoring);

    MaterialData m;
    m.RoughnessFactor = 0.42f;
    REQUIRE(SaveMaterial("materials/brass.amat", m).has_value());

    // Both copies, byte for byte. Without the mirror the authored material lives
    // only in the staged tree a clean build wipes.
    REQUIRE(fs::exists(authoring / "materials" / "brass.amat"));
    CHECK(ReadFile(authoring / "materials" / "brass.amat") == ReadFile(root.path / "materials" / "brass.amat"));

    fs::remove_all(authoring, ec);
}

TEST_CASE("RenameMaterial carries the GUID sidecar with the material")
{
    const AssetRoot root("assisi_matauthor_rename");

    // The sidecar is the whole point of the operation: a material's name is its
    // filename, so renaming must not be the thing that makes every level
    // referencing it stop resolving.
    constexpr std::string_view kSidecar = R"({"guid":"cccccccc-0000-4000-8000-000000000001"})";
    WriteFile(root.path / "materials" / "old.amat", R"({"type":"MaterialData"})");
    WriteFile(root.path / "materials" / "old.amat.aast", kSidecar);

    REQUIRE(Assisi::Geometry::RenameMaterial("materials/old.amat", "materials/new.amat").has_value());

    CHECK(fs::exists(root.path / "materials" / "new.amat"));
    CHECK(ReadFile(root.path / "materials" / "new.amat.aast") == kSidecar);
    CHECK_FALSE(fs::exists(root.path / "materials" / "old.amat"));
    CHECK_FALSE(fs::exists(root.path / "materials" / "old.amat.aast"));
}

TEST_CASE("RenameMaterial refuses to overwrite an existing material")
{
    const AssetRoot root("assisi_matauthor_rename_collide");

    WriteFile(root.path / "a.amat", "A");
    WriteFile(root.path / "b.amat", "B");

    const std::expected<void, MaterialWriteError> result = Assisi::Geometry::RenameMaterial("a.amat", "b.amat");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MaterialWriteError::TargetExists);

    // Neither file moved: a rename that lost the target's contents would be an
    // unrecoverable way to answer "that name is taken".
    CHECK(ReadFile(root.path / "a.amat") == "A");
    CHECK(ReadFile(root.path / "b.amat") == "B");
}

TEST_CASE("RenameMaterial moves the authoring-root copy too")
{
    const AssetRoot root("assisi_matauthor_rename_mirror");
    const fs::path authoring = fs::temp_directory_path() / "assisi_matauthor_rename_mirror_src";
    std::error_code ec;
    fs::remove_all(authoring, ec);
    fs::create_directories(authoring);
    AssetSystem::SetAuthoringRoot(authoring);

    WriteFile(root.path / "old.amat", "BODY");
    WriteFile(root.path / "old.amat.aast", "SIDECAR");
    WriteFile(authoring / "old.amat", "BODY");
    WriteFile(authoring / "old.amat.aast", "SIDECAR");

    REQUIRE(Assisi::Geometry::RenameMaterial("old.amat", "new.amat").has_value());

    // Left behind in the durable tree, the old name comes back on the next build
    // and the rename silently undoes itself.
    CHECK(fs::exists(authoring / "new.amat"));
    CHECK(fs::exists(authoring / "new.amat.aast"));
    CHECK_FALSE(fs::exists(authoring / "old.amat"));
    CHECK_FALSE(fs::exists(authoring / "old.amat.aast"));

    fs::remove_all(authoring, ec);
}

TEST_CASE("DeleteMaterialFile removes the material, its sidecar, and the authoring copy")
{
    const AssetRoot root("assisi_matauthor_delete");
    const fs::path authoring = fs::temp_directory_path() / "assisi_matauthor_delete_src";
    std::error_code ec;
    fs::remove_all(authoring, ec);
    fs::create_directories(authoring);
    AssetSystem::SetAuthoringRoot(authoring);

    WriteFile(root.path / "doomed.amat", "BODY");
    WriteFile(root.path / "doomed.amat.aast", "SIDECAR");
    WriteFile(authoring / "doomed.amat", "BODY");
    WriteFile(authoring / "doomed.amat.aast", "SIDECAR");

    CHECK(Assisi::Geometry::DeleteMaterialFile("doomed.amat"));

    CHECK_FALSE(fs::exists(root.path / "doomed.amat"));
    CHECK_FALSE(fs::exists(root.path / "doomed.amat.aast"));
    CHECK_FALSE(fs::exists(authoring / "doomed.amat"));
    CHECK_FALSE(fs::exists(authoring / "doomed.amat.aast"));

    fs::remove_all(authoring, ec);
}

TEST_CASE("UniqueMaterialPath walks past the names already taken")
{
    const AssetRoot root("assisi_matauthor_unique");

    CHECK(UniqueMaterialPath("materials", "Material") == "materials/Material.amat");

    WriteFile(root.path / "materials" / "Material.amat", "{}");
    CHECK(UniqueMaterialPath("materials", "Material") == "materials/Material_1.amat");

    WriteFile(root.path / "materials" / "Material_1.amat", "{}");
    CHECK(UniqueMaterialPath("materials", "Material") == "materials/Material_2.amat");

    // The asset root itself, where a material has no directory prefix.
    WriteFile(root.path / "Material.amat", "{}");
    CHECK(UniqueMaterialPath("", "Material") == "Material_1.amat");
}
