/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CookTree.hpp
/// @brief Walking the source asset tree once and emitting the cooked one.
///
/// Three properties the cook has to have, and the reason each is not optional:
///
/// **Deterministic.** The same tree cooks to the same bytes, so a content hash
/// is a valid cache key and a patch diff means something. The traps are every
/// container that iterates in an order nobody chose — `AssetDatabase::Assets()`
/// says outright that its order is unspecified — so the walk sorts by virtual
/// path and nothing downstream is allowed to reintroduce a hash order.
///
/// **Incremental.** An unchanged asset is not re-cooked, decided by a key over
/// its bytes, its dependencies' bytes, and the things that change what cooking
/// means: the blob format version, and the layout of whatever codec wrote it. A
/// key over the source alone would leave stale blobs behind every codec change.
///
/// **Total.** A file no cooker claims fails the cook. The alternative is a
/// shipped build quietly missing an asset nobody notices until the level opens,
/// which is exactly what "a cook failure is a build failure" exists to prevent.
/// `.assisiignore` is what says a file is not content in the first place, so the
/// cooker carries no skip list of its own.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include <Assisi/Cook/Cooker.hpp>

namespace Assisi::Cook
{

/// @brief One row of the cooked tree's manifest.
struct ManifestEntry
{
    std::string vpath;
    std::string guid;

    /// What the blob was cooked from, including everything that changes what
    /// cooking means. An entry whose key still matches is skipped.
    std::uint64_t cookKey = 0;

    /// Hash of the bytes produced, so a corrupted or hand-edited cooked tree is
    /// noticed rather than trusted.
    std::uint64_t outputHash = 0;
};

/// @brief What one cook run did.
struct CookReport
{
    std::vector<ManifestEntry> entries; ///< Sorted by virtual path.
    std::size_t cooked  = 0;            ///< Assets whose bytes were written this run.
    std::size_t skipped = 0;            ///< Assets whose key already matched.
    std::size_t sourceOnly = 0;         ///< Files claimed by a cooker that produces nothing.
};

/// @brief Cook @p sourceRoot into @p cookedRoot.
///
/// Reads the existing manifest from @p cookedRoot, if there is one, and skips
/// every asset whose key still matches. Writes the manifest back on success.
///
/// The asset root is scanned read-only: a cook must never mint an id, because an
/// id this process invented is one the authoring tree knows nothing about and
/// the next scan would mint again.
///
/// **This never deletes.** An asset removed from the source tree loses its
/// manifest row — the manifest is rebuilt from the walk each run — but its blob
/// stays in @p cookedRoot forever, and a rename leaves one behind every time,
/// since a rename is a delete and an add. Readers that go through the manifest
/// are unaffected; one that packs the directory itself would pack the orphans.
///
/// @p textureQuality is the compression search tier for textures, and part of each
/// texture's cache key, so switching it re-cooks textures and nothing else.
///
/// @return the report, or the first failure — the cook stops at it rather than
///         carrying on, so the build fails on the first named path instead of
///         burying it under later output.
[[nodiscard]] std::expected<CookReport, CookError>
CookTree(const std::filesystem::path &sourceRoot, const std::filesystem::path &cookedRoot,
         Image::CompressQuality textureQuality = Image::CompressQuality::Best);

/// @brief The manifest's on-disk text: one sorted line per asset.
///
/// Text rather than a blob, and sorted, so two cooked trees diff line by line —
/// which is what makes "the same tree cooks byte-identically" something a person
/// can check rather than take on faith.
[[nodiscard]] std::string SerializeManifest(const std::vector<ManifestEntry> &entries);

/// @brief Parse a manifest. A line that does not read is dropped, which costs a
///        re-cook of that asset and never a wrong skip.
[[nodiscard]] std::vector<ManifestEntry> DeserializeManifest(std::string_view text);

/// @brief The manifest's name inside the cooked tree.
inline constexpr std::string_view kManifestFileName = "manifest.txt";

/// @brief The extension every cooked blob carries.
inline constexpr std::string_view kCookedExtension = ".cooked";

} // namespace Assisi::Cook
