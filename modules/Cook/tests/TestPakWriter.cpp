/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPakWriter.cpp
/// @brief Packing a cooked tree: exactly the manifest's assets, readable back
/// through a mounted pak, and byte-identical from one run to the next.

#include <doctest/doctest.h>

#include <ostream>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <Assisi/Cook/CookTree.hpp>
#include <Assisi/Cook/PakWriter.hpp>
#include <Assisi/Core/PakProvider.hpp>

using Assisi::Cook::CookError;
using Assisi::Cook::CookReport;
using Assisi::Cook::CookTree;
using Assisi::Cook::ManifestEntry;
using Assisi::Cook::PakReport;
using Assisi::Cook::WritePak;
using Assisi::Core::AssetError;
using Assisi::Core::AssetId;
using Assisi::Core::PakCodec;
using Assisi::Core::PakProvider;

namespace
{

/// A scratch directory that cleans itself up.
class ScratchDir
{
public:
    explicit ScratchDir(std::string_view name)
        : _path(std::filesystem::temp_directory_path() / ("assisi-pak-test-" + std::string{name}))
    {
        std::error_code code;
        std::filesystem::remove_all(_path, code);
        std::filesystem::create_directories(_path, code);
    }
    ~ScratchDir()
    {
        std::error_code code;
        std::filesystem::remove_all(_path, code);
    }
    ScratchDir(const ScratchDir &)            = delete;
    ScratchDir &operator=(const ScratchDir &) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const { return _path; }

private:
    std::filesystem::path _path;
};

std::vector<char> ReadFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string Explain(const std::expected<PakReport, CookError> &result)
{
    return result ? std::string{} : result.error().vpath + ": " + result.error().reason;
}

/// Cooks the fixture tree into @p out at the fast tier and returns its manifest.
std::vector<ManifestEntry> CookFixture(const std::filesystem::path &out)
{
    const std::expected<CookReport, CookError> report =
        CookTree(ASSISI_COOK_FIXTURE_ROOT, out, Assisi::Image::CompressQuality::Fast);
    REQUIRE(report.has_value());
    REQUIRE_FALSE(report->entries.empty());
    return report->entries;
}

} // namespace

TEST_CASE("A packed tree serves every manifest asset's cooked bytes, by id and by path")
{
    const ScratchDir cooked("serve-cooked");
    const ScratchDir packed("serve-packed");
    const std::vector<ManifestEntry> manifest = CookFixture(cooked.Path());

    for (const PakCodec codec : {PakCodec::None, PakCodec::Lz4, PakCodec::Zstd})
    {
        CAPTURE(static_cast<std::uint32_t>(codec));
        const std::filesystem::path pakPath = packed.Path() / "assets.pak";
        const std::expected<PakReport, CookError> report = WritePak(cooked.Path(), manifest, pakPath, codec);
        REQUIRE_MESSAGE(report.has_value(), Explain(report));
        CHECK(report->slices == manifest.size());

        const std::expected<PakProvider, AssetError> pak = PakProvider::Mount(pakPath);
        REQUIRE(pak.has_value());
        for (const ManifestEntry &entry : manifest)
        {
            CAPTURE(entry.vpath);
            const AssetId id = *AssetId::Parse(entry.guid);

            const std::expected<AssetId, AssetError> resolved = pak->Resolve(entry.vpath);
            REQUIRE(resolved.has_value());
            CHECK(*resolved == id);

            const std::expected<std::vector<std::byte>, AssetError> bytes = pak->Open(id);
            REQUIRE(bytes.has_value());
            const std::vector<char> blob = ReadFile(cooked.Path() / (entry.guid + ".cooked"));
            REQUIRE(bytes->size() == blob.size());
            CHECK(std::equal(blob.begin(), blob.end(), bytes->begin(),
                             [](char c, std::byte b) { return static_cast<std::byte>(c) == b; }));
        }
    }
}

TEST_CASE("A blob the manifest does not list is left out of the pak")
{
    // The cooker never deletes: a removed or renamed asset leaves its blob in the
    // cooked directory forever. Packing the directory would ship those.
    const ScratchDir cooked("orphan-cooked");
    const ScratchDir packed("orphan-packed");
    const std::vector<ManifestEntry> manifest = CookFixture(cooked.Path());

    const AssetId orphan = Assisi::Core::DerivedAssetId("orphan");
    std::filesystem::copy_file(cooked.Path() / (manifest.front().guid + ".cooked"),
                               cooked.Path() / (orphan.ToString() + ".cooked"));

    const std::filesystem::path pakPath = packed.Path() / "assets.pak";
    REQUIRE(WritePak(cooked.Path(), manifest, pakPath, PakCodec::None).has_value());

    const std::expected<PakProvider, AssetError> pak = PakProvider::Mount(pakPath);
    REQUIRE(pak.has_value());
    const std::expected<std::vector<std::byte>, AssetError> bytes = pak->Open(orphan);
    REQUIRE_FALSE(bytes.has_value());
    CHECK(bytes.error() == AssetError::UnknownAssetId);
}

TEST_CASE("A manifest row whose blob is missing fails the pack, naming the path")
{
    const ScratchDir cooked("missing-cooked");
    const ScratchDir packed("missing-packed");
    const std::vector<ManifestEntry> manifest = CookFixture(cooked.Path());
    std::filesystem::remove(cooked.Path() / (manifest.back().guid + ".cooked"));

    const std::expected<PakReport, CookError> report =
        WritePak(cooked.Path(), manifest, packed.Path() / "assets.pak", PakCodec::None);
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().vpath == manifest.back().vpath);
    // Nothing half-written is left where a game would find it.
    CHECK_FALSE(std::filesystem::exists(packed.Path() / "assets.pak"));
}

TEST_CASE("Two paths deriving one path id fail the pack, naming both")
{
    // A lookup by path goes through the path's derived id, so two paths sharing
    // one would make one of the assets unreachable by path. The fixture has no
    // real collision, so one path is listed twice under two ids.
    const ScratchDir cooked("collide-cooked");
    const ScratchDir packed("collide-packed");
    std::vector<ManifestEntry> manifest = CookFixture(cooked.Path());
    REQUIRE(manifest.size() >= 2);
    manifest[1].vpath = manifest[0].vpath;

    const std::expected<PakReport, CookError> report =
        WritePak(cooked.Path(), manifest, packed.Path() / "assets.pak", PakCodec::None);
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().reason.find(manifest[0].vpath) != std::string::npos);
}

TEST_CASE("Packing the same tree twice produces identical bytes")
{
    // A patch is a diff between two builds' paks, so a pack that differs between
    // runs of the same input makes every patch the whole game.
    const ScratchDir cooked("determinism-cooked");
    const ScratchDir packed("determinism-packed");
    const std::vector<ManifestEntry> manifest = CookFixture(cooked.Path());

    REQUIRE(WritePak(cooked.Path(), manifest, packed.Path() / "a.pak", PakCodec::Zstd).has_value());
    REQUIRE(WritePak(cooked.Path(), manifest, packed.Path() / "b.pak", PakCodec::Zstd).has_value());
    CHECK(ReadFile(packed.Path() / "a.pak") == ReadFile(packed.Path() / "b.pak"));
}
