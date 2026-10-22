/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PakWriterTesting.hpp
/// @brief Cooked trees to pack, and ways to look inside the paks packed from
///        them: what each pak-writer test builds on.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <Assisi/Cook/CookTree.hpp>
#include <Assisi/Cook/PakWriter.hpp>
#include <Assisi/Core/PakFormat.hpp>

namespace Assisi::Cook::Testing
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
    ScratchDir(const ScratchDir &) = delete;
    ScratchDir &operator=(const ScratchDir &) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const { return _path; }

private:
    std::filesystem::path _path;
};

inline constexpr std::uint64_t kKiB = 1024;
inline constexpr std::uint64_t kMiB = kKiB * kKiB;

/// The chunk a storefront's delta updater cuts a file into, at fixed offsets.
inline constexpr std::uint64_t kChunkBytes = kMiB;

/// A slice well under a chunk, so several share one.
inline constexpr std::uint64_t kSliceBytes = 150 * kKiB;

/// Enough slices to span a dozen chunks, so a layout that shifts everything after
/// a change ships far more chunks than one that moves a single slice.
inline constexpr std::uint32_t kSliceCount = 80;

std::vector<char> ReadFile(const std::filesystem::path &path);

std::string Explain(const std::expected<PakReport, CookError> &result);

/// Cooks the fixture tree into @p out at the fast tier and returns its manifest.
std::vector<ManifestEntry> CookFixture(const std::filesystem::path &out);

/// Writes a cooked blob of exactly @p totalBytes, header included, under @p cookedRoot
/// and returns the manifest row naming it. The payload depends on @p vpath, so no
/// two blobs share bytes and a chunk's content says which slices it holds.
ManifestEntry WriteBlob(const std::filesystem::path &cookedRoot, const std::string &vpath, std::uint64_t totalBytes);

/// Every index row of the pak at @p path, in index order.
std::vector<Assisi::Core::PakEntry> ReadIndex(const std::filesystem::path &path);

/// Chunks of @p after that @p before does not hold anywhere: what an update ships.
std::size_t NewChunks(const std::filesystem::path &before, const std::filesystem::path &after);

/// Packs @p manifest into @p outPath over the pak at @p previousPath.
std::expected<PakReport, CookError> PackOver(const std::filesystem::path &cookedRoot,
                                             std::span<const ManifestEntry> manifest,
                                             const std::filesystem::path &outPath,
                                             const std::filesystem::path &previousPath);

/// Mounts the pak at @p pakPath as a game would and checks that every asset in
/// @p manifest comes back as exactly the blob cooked for it. An index row that
/// disagrees with where its bytes were written, or two slices written over each
/// other, fails here even when every offset looks right.
void CheckServesEveryAsset(const std::filesystem::path &pakPath, const std::filesystem::path &cookedRoot,
                           std::span<const ManifestEntry> manifest);

/// The index row for @p vpath in @p index.
const Assisi::Core::PakEntry &Row(const std::vector<Assisi::Core::PakEntry> &index, const std::string &vpath);

/// A manifest of @p count equal slices, named so they sort in creation order.
std::vector<ManifestEntry> WriteEqualBlobs(const std::filesystem::path &cookedRoot, std::uint32_t count,
                                           std::uint64_t bytesEach);

} // namespace Assisi::Cook::Testing
