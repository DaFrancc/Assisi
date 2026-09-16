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

#include <Assisi/Cook/CookTree.hpp>
#include <Assisi/Cook/Cooker.hpp>
#include <Assisi/Core/PakCodec.hpp>

namespace Assisi::Cook
{

/// @brief What one pack wrote.
struct PakReport
{
    std::uint64_t storedBytes       = 0; ///< Slice bytes on disk, after compression.
    std::uint64_t uncompressedBytes = 0; ///< Slice bytes a loader receives.
    std::size_t slices              = 0;
};

/// @brief Pack the blobs @p entries lists from @p cookedRoot into @p outPath.
///
/// Slices are written in manifest order, which is sorted by virtual path, so one
/// directory's assets sit together in the file. Each is compressed with @p codec
/// unless that does not make it smaller, in which case it is stored as cooked.
///
/// The archive is written beside @p outPath and renamed into place only once it
/// is complete, so a failed pack never leaves a partial pak for a game to mount.
///
/// @return the report, or the first failure: a listed blob that is missing or
///         unreadable, or two paths sharing one derived path id (both named).
[[nodiscard]] std::expected<PakReport, CookError> WritePak(const std::filesystem::path &cookedRoot,
                                                           std::span<const ManifestEntry> entries,
                                                           const std::filesystem::path &outPath, Core::PakCodec codec);

} // namespace Assisi::Cook
