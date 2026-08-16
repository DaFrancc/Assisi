/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file LooseFileProvider.hpp
/// @brief The editor `AssetProvider` backend: id → path (via the database) →
///        loose file on disk.
///
/// One of the two `AssetProvider` backends (the shipped one is PakProvider, S5).
/// It resolves an `AssetId` to a virtual path through a live `AssetDatabase`,
/// then reads that file through `AssetSystem`. Reserved built-in ids are not
/// served here — they are primitives resolved above the provider (the render
/// AssetCache), so opening one is an error.
///
/// Editor-only: it depends on the mutable database and the loose asset tree,
/// neither of which exists in a shipped build.

#include <cstddef>
#include <expected>
#include <vector>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetProvider.hpp>
#include <Assisi/Core/Errors.hpp>

namespace Assisi::Core
{

class AssetDatabase;

/// @brief `AssetProvider` over loose files, resolved through an `AssetDatabase`.
class LooseFileProvider final : public AssetProvider
{
public:
    /// @param database The GUID→path index to resolve through. Must outlive this
    ///        provider (the provider holds a reference, not a copy).
    explicit LooseFileProvider(const AssetDatabase &database) noexcept;

    /// @brief Resolve @p id to a path via the database and read the file's bytes.
    /// @return The bytes, or AssetError::UnknownAssetId if the id is reserved or
    ///         not in the database (FileOpenFailed / FileReadFailed on I/O).
    [[nodiscard]] std::expected<std::vector<std::byte>, AssetError> Open(AssetId id) const override;

private:
    const AssetDatabase *_database;
};

} // namespace Assisi::Core
