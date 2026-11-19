/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPakLayout.cpp
/// @brief A pak laid out fresh, or over its own previous release unchanged: slices
///        back to back, and the same bytes again. A previous release in another
///        layout version gives nothing to keep.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <vector>

#include "PakWriterTesting.hpp"

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/PakFormat.hpp>
#include <Assisi/Core/PakProvider.hpp>

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

TEST_CASE("With no previous pak, slices sit back to back in manifest order")
{
    const ScratchDir cooked("fresh-cooked");
    const ScratchDir packed("fresh-packed");
    constexpr std::uint64_t kTinyBytes = 64;
    const std::vector<ManifestEntry> manifest = {WriteBlob(cooked.Path(), "a", 100 * kKiB),
                                                 WriteBlob(cooked.Path(), "b", kTinyBytes),
                                                 WriteBlob(cooked.Path(), "c", 300 * kKiB)};

    const std::filesystem::path pakPath = packed.Path() / "assets.pak";
    const std::expected<PakReport, CookError> report = WritePak(cooked.Path(), manifest, pakPath, PakCodec::None, {});
    REQUIRE_MESSAGE(report.has_value(), Explain(report));
    CHECK(report->layout == PakLayout::Fresh);
    CHECK(report->gapBytes == 0);

    const std::vector<Assisi::Core::PakEntry> index = ReadIndex(pakPath);
    CHECK(Row(index, "a").offset == Assisi::Core::kPakHeaderBytes);
    CHECK(Row(index, "b").offset == Assisi::Core::kPakHeaderBytes + 100 * kKiB);
    CHECK(Row(index, "c").offset == Assisi::Core::kPakHeaderBytes + 100 * kKiB + kTinyBytes);
}

TEST_CASE("Repacking an unchanged tree over its own pak reproduces it byte for byte")
{
    const ScratchDir cooked("same-cooked");
    const ScratchDir packed("same-packed");
    const std::vector<ManifestEntry> manifest = WriteEqualBlobs(cooked.Path(), kSliceCount, kSliceBytes);
    REQUIRE(WritePak(cooked.Path(), manifest, packed.Path() / "before.pak", PakCodec::None, {}).has_value());

    const std::expected<PakReport, CookError> report =
        PackOver(cooked.Path(), manifest, packed.Path() / "after.pak", packed.Path() / "before.pak");
    REQUIRE_MESSAGE(report.has_value(), Explain(report));
    CHECK(report->layout == PakLayout::FromPrevious);
    CHECK(report->keptSlices == manifest.size());
    CHECK(ReadFile(packed.Path() / "before.pak") == ReadFile(packed.Path() / "after.pak"));
}

TEST_CASE("A previous pak of another layout version gives no layout to keep, and is no failure")
{
    // Every release made before a pak format change is in the older version. A
    // pack that refused it would stop every package until the releases were
    // moved aside; one that read it would place slices over the new header.
    const ScratchDir cooked("old-version-cooked");
    const ScratchDir packed("old-version-packed");
    const std::vector<ManifestEntry> manifest = {WriteBlob(cooked.Path(), "a", kSliceBytes)};

    const std::filesystem::path older = packed.Path() / "older.pak";
    {
        Assisi::Core::PakHeader header;
        header.version = Assisi::Core::kPakFormatVersion - 1;
        Assisi::Core::BitWriter writer;
        Assisi::Core::WritePakHeader(writer, header);
        std::ofstream out(older, std::ios::binary | std::ios::trunc);
        const std::span<const std::byte> bytes = writer.Data();
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(out.good());
    }
    const std::expected<std::optional<PakProvider>, AssetError> fromOlder = Assisi::Cook::MountPreviousPak(older);
    REQUIRE(fromOlder.has_value());
    CHECK_FALSE(fromOlder->has_value());

    // The current version still mounts, so a release made by this build keeps
    // its layout in the next update.
    const std::filesystem::path current = packed.Path() / "current.pak";
    REQUIRE(WritePak(cooked.Path(), manifest, current, PakCodec::None, {}).has_value());
    const std::expected<std::optional<PakProvider>, AssetError> fromCurrent = Assisi::Cook::MountPreviousPak(current);
    REQUIRE(fromCurrent.has_value());
    CHECK(fromCurrent->has_value());

    // And a file that is no pak at all is still refused, not quietly ignored.
    const std::filesystem::path garbage = packed.Path() / "garbage.pak";
    {
        std::ofstream out(garbage, std::ios::binary | std::ios::trunc);
        out << "not a pak, and long enough to hold a header";
    }
    CHECK_FALSE(Assisi::Cook::MountPreviousPak(garbage).has_value());
}
