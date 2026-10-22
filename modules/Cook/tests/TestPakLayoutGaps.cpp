/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPakLayoutGaps.cpp
/// @brief Gaps a removed slice leaves are zeroed and filled by the smallest fit, and
///        too many are compacted away.

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

TEST_CASE("A removed slice is zeroed, and a new slice takes the smallest gap it fits")
{
    const ScratchDir cooked("gap-cooked");
    const ScratchDir packed("gap-packed");
    std::vector<ManifestEntry> manifest = WriteEqualBlobs(cooked.Path(), kSliceCount, kSliceBytes);
    const std::string small = manifest[5].vpath;
    const std::string large = manifest[12].vpath;
    WriteBlob(cooked.Path(), large, 2 * kSliceBytes);
    const std::filesystem::path first = packed.Path() / "first.pak";
    const std::filesystem::path second = packed.Path() / "second.pak";
    const std::filesystem::path third = packed.Path() / "third.pak";
    REQUIRE(WritePak(cooked.Path(), manifest, first, PakCodec::None, {}).has_value());
    const std::vector<Assisi::Core::PakEntry> firstIndex = ReadIndex(first);

    std::erase_if(manifest, [&](const ManifestEntry &entry) { return entry.vpath == small || entry.vpath == large; });
    const std::expected<PakReport, CookError> removed = PackOver(cooked.Path(), manifest, second, first);
    REQUIRE_MESSAGE(removed.has_value(), Explain(removed));
    CHECK(removed->layout == PakLayout::FromPrevious);
    CHECK(removed->gapBytes == 3 * kSliceBytes);

    // Content a release deleted does not ship in the next one.
    const std::vector<char> secondBytes = ReadFile(second);
    const Assisi::Core::PakEntry &gone = Row(firstIndex, small);
    CHECK(std::all_of(secondBytes.begin() + static_cast<std::ptrdiff_t>(gone.offset),
                      secondBytes.begin() + static_cast<std::ptrdiff_t>(gone.offset + gone.storedSize),
                      [](char c) { return c == 0; }));
    CheckServesEveryAsset(second, cooked.Path(), manifest);

    constexpr std::uint64_t kAddedBytes = 100 * kKiB;
    manifest.push_back(WriteBlob(cooked.Path(), "slice999", kAddedBytes));
    const std::expected<PakReport, CookError> added = PackOver(cooked.Path(), manifest, third, second);
    REQUIRE_MESSAGE(added.has_value(), Explain(added));
    CHECK(Row(ReadIndex(third), "slice999").offset == gone.offset);
    CHECK(added->gapBytes == 3 * kSliceBytes - kAddedBytes);
    CheckServesEveryAsset(third, cooked.Path(), manifest);
}

TEST_CASE("Gaps past the limit are compacted into a fresh layout")
{
    const ScratchDir cooked("compact-cooked");
    const ScratchDir packed("compact-packed");
    std::vector<ManifestEntry> manifest = WriteEqualBlobs(cooked.Path(), kSliceCount, kSliceBytes);
    REQUIRE(WritePak(cooked.Path(), manifest, packed.Path() / "before.pak", PakCodec::None, {}).has_value());

    // A fifth of the slices, from the front, so what they leave is gaps rather
    // than space past the last slice.
    constexpr std::size_t kRemoved = kSliceCount / 5;
    manifest.erase(manifest.begin(), manifest.begin() + kRemoved);
    const std::expected<PakReport, CookError> report =
        PackOver(cooked.Path(), manifest, packed.Path() / "after.pak", packed.Path() / "before.pak");
    REQUIRE_MESSAGE(report.has_value(), Explain(report));
    CHECK(report->layout == PakLayout::Compacted);
    CHECK(report->gapBytes == 0);
    CHECK(Row(ReadIndex(packed.Path() / "after.pak"), manifest.front().vpath).offset == Assisi::Core::kPakHeaderBytes);
    CheckServesEveryAsset(packed.Path() / "after.pak", cooked.Path(), manifest);
}
