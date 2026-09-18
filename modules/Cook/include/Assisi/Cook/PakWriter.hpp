/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PakWriter.hpp
/// @brief Packing a cooked tree into one pak archive.
///
/// **The manifest decides what ships, not the directory.** The cook never deletes:
/// an asset removed from the source tree loses its manifest row, but its blob
/// stays in the cooked directory, and a rename leaves one behind every time.
/// Packing the directory would ship every one of those orphans.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>

#include <Assisi/Cook/CookTree.hpp>
#include <Assisi/Cook/Cooker.hpp>
#include <Assisi/Core/PakCodec.hpp>
#include <Assisi/Core/PakFormat.hpp>

namespace Assisi::Cook
{

/// @brief How a pack placed its slices.
enum class PakLayout : std::uint8_t
{
    Fresh,        ///< No previous pak: every slice back to back, in manifest order.
    FromPrevious, ///< Laid out over the previous pak's placements.
    Compacted,    ///< A previous pak was given, but its layout left too much unused; laid out fresh.
    Count,
};

/// @brief What one pack wrote.
struct PakReport
{
    std::uint64_t storedBytes       = 0; ///< Slice bytes on disk, after compression.
    std::uint64_t uncompressedBytes = 0; ///< Slice bytes a loader receives.
    std::uint64_t gapBytes          = 0; ///< Zeros between slices, where slices were removed or moved.
    std::size_t slices              = 0;
    std::size_t keptSlices          = 0; ///< Slices at the offset the previous pak had them.
    PakLayout layout                = PakLayout::Fresh;
};

/// @brief The share of a pak's slice region, in percent, that may be gaps before a
///        pack lays it out fresh. A fresh layout ships as a full download, so gaps
///        are tolerated well past the point where they would be noticed on disk.
inline constexpr std::uint64_t kMaxPakGapPercent = 10;

/// @brief Pack the blobs @p entries lists from @p cookedRoot into @p outPath.
///
/// Each slice is compressed with @p codec unless that does not make it smaller,
/// in which case it is stored as cooked.
///
/// With no @p previous entries, slices are written back to back in manifest
/// order, which is sorted by virtual path, so one directory's assets sit together.
///
/// With @p previous — the index of the pak the last release shipped — no slice
/// moves unless it has to, because a storefront's delta updater compares files in
/// fixed chunks and any byte that shifts makes every chunk after it new:
///   - a slice no larger than before stays at its old offset;
///   - a new slice, or one that outgrew its place, goes into the smallest gap it
///     fits, largest slices first, or at the end;
///   - where a removed or shrunk slice was, zeros are written, so content a
///     release deleted does not ship in the next one.
/// When the gaps left over exceed kMaxPakGapPercent of the slice region, the
/// previous layout is discarded and the pak is laid out fresh.
///
/// The archive is written beside @p outPath and renamed into place only once it
/// is complete, so a failed pack never leaves a partial pak for a game to mount.
///
/// @return the report, or the first failure: a listed blob that is missing or
///         unreadable, two paths sharing one derived path id (both named), or a
///         previous index whose slices overlap.
[[nodiscard]] std::expected<PakReport, CookError> WritePak(const std::filesystem::path &cookedRoot,
                                                           std::span<const ManifestEntry> entries,
                                                           const std::filesystem::path &outPath, Core::PakCodec codec,
                                                           std::span<const Core::PakEntry> previous);

/// @brief A short name for @p layout, for the pack tool's report line.
[[nodiscard]] std::string_view ToString(PakLayout layout) noexcept;

} // namespace Assisi::Cook
