/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PakProvider.hpp
/// @brief The shipped `AssetProvider` backend: cooked slices of one pak archive.
///
/// Mounting reads the header and the whole index in two reads and keeps the file
/// open; no asset bytes are read until something asks for them. `Open` is then a
/// bounded read of one slice, decompressed on the calling thread.
///
/// **No fall-through.** An id the index does not hold is UnknownAssetId. A shipped
/// build that quietly read a loose file when the pak was incomplete would hide
/// exactly the packaging mistake this exists to surface.

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <Assisi/Core/AssetProvider.hpp>
#include <Assisi/Core/PakFormat.hpp>
#include <Assisi/Core/RandomAccessFile.hpp>

namespace Assisi::Core
{

class PakProvider final : public AssetProvider
{
public:
    /// @brief Open the pak at @p path and read its index.
    ///
    /// Every entry is checked against the file before anything is served: a
    /// slice that runs past the end of the file, overlaps the header or the
    /// index, or shares an id or a path with another refuses the whole archive,
    /// because an index that wrong is not one whose other rows can be trusted.
    ///
    /// @return the provider, or FileOpenFailed / CorruptArchive /
    ///         UnsupportedEncoding (a layout version this build does not read).
    [[nodiscard]] static std::expected<PakProvider, AssetError> Mount(const std::filesystem::path &path);

    /// @brief The asset's bytes as cooked: read, then decompressed.
    ///
    /// UnsupportedEncoding for a slice that is encrypted, uses a codec this build
    /// lacks, or names another archive; CorruptArchive if it does not decompress
    /// to the size the index records.
    [[nodiscard]] std::expected<std::vector<std::byte>, AssetError> Open(AssetId id) const override;

    /// @brief The id packed under @p vpath, found by its DerivedAssetId.
    [[nodiscard]] std::expected<AssetId, AssetError> Resolve(std::string_view vpath) const override;

    /// @brief Every entry of @p kind, ordered by id so the order is a property of
    ///        the pak rather than of how the index was hashed.
    [[nodiscard]] std::vector<PakEntry> EntriesOfKind(CookedKind kind) const;

private:
    PakProvider(RandomAccessFile file, std::vector<PakEntry> entries);

    RandomAccessFile _file;
    std::vector<PakEntry> _entries;
    std::unordered_map<AssetId, std::size_t> _byId;
    std::unordered_map<AssetId, std::size_t> _byPath;
};

} // namespace Assisi::Core
