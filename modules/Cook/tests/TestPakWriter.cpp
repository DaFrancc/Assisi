/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPakWriter.cpp
/// @brief Packing a cooked tree: exactly the manifest's assets, readable back
///        through a mounted pak, and byte-identical from one run to the next.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "PakWriterTesting.hpp"

#include <Assisi/Core/AssetStore.hpp>
#include <Assisi/Core/JobSystem.hpp>
#include <Assisi/Core/PakProvider.hpp>
#include <Assisi/Testing/TestAssetKinds.hpp>

using namespace Assisi::Cook::Testing;
using Assisi::Cook::CookError;
using Assisi::Cook::ManifestEntry;
using Assisi::Cook::PakLayout;
using Assisi::Cook::PakReport;
using Assisi::Cook::WritePak;
using Assisi::Core::AssetError;
using Assisi::Core::AssetErrorCode;
using Assisi::Core::AssetId;
using Assisi::Core::PakCodec;
using Assisi::Core::PakProvider;

TEST_CASE("A packed tree serves every manifest asset's cooked bytes, by id and by path")
{
    const ScratchDir cooked("serve-cooked");
    const ScratchDir packed("serve-packed");
    const std::vector<ManifestEntry> manifest = CookFixture(cooked.Path());

    for (const PakCodec codec : {PakCodec::None, PakCodec::Lz4, PakCodec::Zstd})
    {
        CAPTURE(static_cast<std::uint32_t>(codec));
        const std::filesystem::path pakPath = packed.Path() / "assets.pak";
        const std::expected<PakReport, CookError> report = WritePak(cooked.Path(), manifest, pakPath, codec, {});
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

TEST_CASE("A registered kind's asset is loaded by id from a pak, as a game loads it")
{
    // End to end for a kind nothing in the engine names: cooked by its own step,
    // packed, then read back by id through the store a game reads through.
    const ScratchDir cooked("kind-cooked");
    const ScratchDir packed("kind-packed");
    const std::vector<ManifestEntry> manifest = CookFixture(cooked.Path());
    const std::filesystem::path pakPath = packed.Path() / "assets.pak";
    REQUIRE(WritePak(cooked.Path(), manifest, pakPath, PakCodec::Zstd, {}).has_value());

    const std::expected<PakProvider, AssetError> pak = PakProvider::Mount(pakPath);
    REQUIRE(pak.has_value());
    const std::expected<AssetId, AssetError> id = pak->Resolve("things/sample.tbytes");
    REQUIRE(id.has_value());

    constexpr std::uint32_t kWorkers = 2;
    Assisi::Core::JobSystem jobs(kWorkers);
    Assisi::Core::AssetStore store;
    store.Initialize(jobs, *pak);
    (void)store.Resolve<Assisi::Testing::TestBytes>(*id);
    jobs.HelpUntil([&store] { return !store.HasPendingLoads(); }, true);

    const std::shared_ptr<const Assisi::Testing::TestBytes> loaded = store.Resolve<Assisi::Testing::TestBytes>(*id);
    REQUIRE(loaded != nullptr);
    // The fixture holds "abc", which the kind's cook step reversed.
    const std::string text{reinterpret_cast<const char *>(loaded->bytes.data()), loaded->bytes.size()};
    CHECK(text == "cba");
    CHECK(loaded->finished);

    // A PNG whose sidecar says it is the raw test kind loads as that kind, not
    // as a texture: its payload is the PNG file's bytes.
    const std::expected<AssetId, AssetError> photoId = pak->Resolve("things/photo.png");
    REQUIRE(photoId.has_value());
    (void)store.Resolve<Assisi::Testing::TestBytes>(*photoId);
    jobs.HelpUntil([&store] { return !store.HasPendingLoads(); }, true);
    const std::shared_ptr<const Assisi::Testing::TestBytes> photo = store.Resolve<Assisi::Testing::TestBytes>(*photoId);
    REQUIRE(photo != nullptr);
    const std::vector<char> png = ReadFile(std::filesystem::path{ASSISI_COOK_FIXTURE_ROOT} / "things" / "photo.png");
    REQUIRE_FALSE(png.empty());
    CHECK(photo->bytes.size() == png.size());
    CHECK(std::ranges::equal(photo->bytes, std::as_bytes(std::span{png})));
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
    REQUIRE(WritePak(cooked.Path(), manifest, pakPath, PakCodec::None, {}).has_value());

    const std::expected<PakProvider, AssetError> pak = PakProvider::Mount(pakPath);
    REQUIRE(pak.has_value());
    const std::expected<std::vector<std::byte>, AssetError> bytes = pak->Open(orphan);
    REQUIRE_FALSE(bytes.has_value());
    CHECK(bytes.error() == AssetErrorCode::UnknownAssetId);
}

TEST_CASE("A manifest row whose blob is missing fails the pack, naming the path")
{
    const ScratchDir cooked("missing-cooked");
    const ScratchDir packed("missing-packed");
    const std::vector<ManifestEntry> manifest = CookFixture(cooked.Path());
    std::filesystem::remove(cooked.Path() / (manifest.back().guid + ".cooked"));

    const std::expected<PakReport, CookError> report =
        WritePak(cooked.Path(), manifest, packed.Path() / "assets.pak", PakCodec::None, {});
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
        WritePak(cooked.Path(), manifest, packed.Path() / "assets.pak", PakCodec::None, {});
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

    REQUIRE(WritePak(cooked.Path(), manifest, packed.Path() / "a.pak", PakCodec::Zstd, {}).has_value());
    REQUIRE(WritePak(cooked.Path(), manifest, packed.Path() / "b.pak", PakCodec::Zstd, {}).has_value());
    CHECK(ReadFile(packed.Path() / "a.pak") == ReadFile(packed.Path() / "b.pak"));
}
