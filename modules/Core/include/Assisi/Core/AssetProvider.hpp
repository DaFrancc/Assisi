/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetProvider.hpp
/// @brief The GUID→bytes seam: one interface, two backends.
///
/// Resolving an `AssetId` to raw bytes is not one operation. In the editor,
/// assets are loose files with `.aast` sidecars resolved through a live,
/// mutable database (LooseFileProvider). In a shipped build, assets are packed
/// into archives and the sidecars do not exist — the bytes come from a slice of
/// an archive addressed by a baked index (PakProvider, a later stage). The
/// storage and the lookup differ; what is identical is everything *above* the
/// bytes — parsing `.amat` JSON, uploading a mesh, resolving texture channels.
///
/// So the seam is this interface. Deserializers and the render `AssetCache` are
/// written against it and never learn which backend served them. See
/// docs/asset-database-architecture.md §3.
///
/// This interface ships in every build. Its editor backend (LooseFileProvider)
/// does not.

#include <cstddef>
#include <expected>
#include <vector>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/Errors.hpp>

namespace Assisi::Core
{

/// @brief Abstract source of asset bytes, addressed by stable id.
class AssetProvider
{
public:
    virtual ~AssetProvider() = default;

    /// @brief Read the full byte payload of the asset with this id.
    ///
    /// @param id The asset to open. Reserved built-in ids (the `prim://`
    ///        primitives) are handled by the resolver above this layer, not by a
    ///        provider — opening one returns AssetError::UnknownAssetId.
    /// @return The bytes, or an AssetError (UnknownAssetId if the id is not
    ///         known to this provider; FileOpenFailed / FileReadFailed on I/O
    ///         trouble).
    [[nodiscard]] virtual std::expected<std::vector<std::byte>, AssetError> Open(AssetId id) const = 0;
};

} // namespace Assisi::Core
