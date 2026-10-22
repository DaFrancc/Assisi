/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPakLayout.cpp
/// @brief A pak laid out fresh, or over its own previous release unchanged: slices
///        back to back, and the same bytes again.

#include <doctest/doctest.h>

#include <cstdint>
#include <expected>
#include <filesystem>
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
