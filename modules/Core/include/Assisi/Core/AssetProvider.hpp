/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetProvider.hpp
/// @brief The GUID→bytes seam: one interface, two backends.
///
/// The editor reads loose files, resolved through the mutable database built from
/// `.aast` sidecars (LooseFileProvider). A shipped game reads slices of a pak,
/// addressed by the index baked into it (PakProvider), and has neither sidecars
/// nor loose files. The storage and the lookup differ; the question asked is the
/// same.
///
/// What the bytes *are* differs too: source files from one, cooked blobs from the
/// other. Decoding them is the layer above, which each executable installs to
/// match the provider it built.

#include <cstddef>
#include <expected>
#include <string_view>
#include <vector>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/Errors.hpp>

namespace Assisi::Core
{

/// @brief Abstract source of asset bytes, addressed by stable id.
///
/// Both calls may run on worker threads concurrently, so an implementation holds
/// no per-call state.
class AssetProvider
{
public:
    virtual ~AssetProvider() = default;

    /// @brief Read the full byte payload of the asset with this id.
    ///
    /// @param id The asset to open. Reserved built-in ids (the `prim://`
    ///        primitives) are handled by the resolver above this layer, not by a
    ///        provider — opening one returns AssetError::UnknownAssetId.
    /// @return The bytes, or an AssetError: UnknownAssetId if this provider does
    ///         not serve the id, FileOpenFailed / FileReadFailed on I/O trouble,
    ///         and UnsupportedEncoding / CorruptArchive for a slice the pak cannot
    ///         decode. Never a fall-through to another source.
    [[nodiscard]] virtual std::expected<std::vector<std::byte>, AssetError> Open(AssetId id) const = 0;

    /// @brief The id of the asset at @p vpath.
    ///
    /// For callers that address an asset by path — a config file, a shader, a
    /// level named in a config. UnknownAssetId when this provider has no such path.
    [[nodiscard]] virtual std::expected<AssetId, AssetError> Resolve(std::string_view vpath) const = 0;
};

} // namespace Assisi::Core
