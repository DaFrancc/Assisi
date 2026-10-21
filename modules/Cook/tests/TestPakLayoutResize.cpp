/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPakLayoutResize.cpp
/// @brief A slice that changes size over the previous release's pak moves only
///        itself, or stays put and leaves a gap.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "PakWriterTesting.hpp"

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

TEST_CASE("A slice that grows moves, and every other slice keeps its offset")
{
    // A delta updater cuts files at fixed offsets. A slice that grows must not shift
    // every byte after it, or one changed material ships the rest of the pak.
    const ScratchDir cooked("grow-cooked");
    const ScratchDir packed("grow-packed");
    const std::vector<ManifestEntry> manifest = WriteEqualBlobs(cooked.Path(), kSliceCount, kSliceBytes);
    const std::filesystem::path before = packed.Path() / "before.pak";
    const std::filesystem::path after = packed.Path() / "after.pak";
    REQUIRE(WritePak(cooked.Path(), manifest, before, PakCodec::None, {}).has_value());

    constexpr std::uint64_t kGrowthBytes = 8;
    const std::string grown = manifest[2].vpath;
    WriteBlob(cooked.Path(), grown, kSliceBytes + kGrowthBytes);
    const std::expected<PakReport, CookError> report = PackOver(cooked.Path(), manifest, after, before);
    REQUIRE_MESSAGE(report.has_value(), Explain(report));
    CHECK(report->keptSlices == manifest.size() - 1);

    const std::vector<Assisi::Core::PakEntry> oldIndex = ReadIndex(before);
    const std::vector<Assisi::Core::PakEntry> newIndex = ReadIndex(after);
    for (const ManifestEntry &entry : manifest)
    {
        CAPTURE(entry.vpath);
        if (entry.vpath != grown)
        {
            CHECK(Row(newIndex, entry.vpath).offset == Row(oldIndex, entry.vpath).offset);
        }
    }
    // Nothing else is big enough to take it, so it goes where the slices ended.
    const Assisi::Core::PakEntry &last = Row(oldIndex, manifest.back().vpath);
    CHECK(Row(newIndex, grown).offset == last.offset + last.storedSize);

    // The header's chunk, up to two where the slice was and is now zeros, and up to
    // two where it now sits beside the index.
    constexpr std::size_t kChunksPerMovedSlice = 5;
    CHECK(NewChunks(before, after) <= kChunksPerMovedSlice);

    CheckServesEveryAsset(after, cooked.Path(), manifest);
}

TEST_CASE("A slice that shrinks stays where it was, leaving the rest of its place as a gap")
{
    const ScratchDir cooked("shrink-cooked");
    const ScratchDir packed("shrink-packed");
    const std::vector<ManifestEntry> manifest = WriteEqualBlobs(cooked.Path(), kSliceCount, kSliceBytes);
    const std::filesystem::path before = packed.Path() / "before.pak";
    const std::filesystem::path after = packed.Path() / "after.pak";
    REQUIRE(WritePak(cooked.Path(), manifest, before, PakCodec::None, {}).has_value());

    WriteBlob(cooked.Path(), manifest[2].vpath, kSliceBytes - kKiB);
    const std::expected<PakReport, CookError> report = PackOver(cooked.Path(), manifest, after, before);
    REQUIRE_MESSAGE(report.has_value(), Explain(report));
    CHECK(report->keptSlices == manifest.size());
    CHECK(report->gapBytes == kKiB);
    CHECK(Row(ReadIndex(after), manifest[2].vpath).offset == Row(ReadIndex(before), manifest[2].vpath).offset);

    CheckServesEveryAsset(after, cooked.Path(), manifest);
}
