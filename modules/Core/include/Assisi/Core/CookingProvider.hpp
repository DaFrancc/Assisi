/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CookingProvider.hpp
/// @brief Cooked blobs made on demand from source files, for every host that
///        works on the source tree rather than a package.
///
/// A game reads blobs the cook wrote into its package. The editor and tools
/// have the source files, so this runs the same cook, in memory, when a blob is
/// asked for: the file's extension picks its registered kind, and the kind's
/// cook step turns the source into the payload. An AssetStore reads through
/// either one the same way, so there is one load path whichever host it is.
///
/// Only registered kinds are served. The engine's own kinds keep their own
/// source readers.

#include <Assisi/Core/AssetProvider.hpp>

#include <functional>
#include <optional>
#include <string>

namespace Assisi::Core
{

class CookingProvider final : public AssetProvider
{
  public:
    /// @brief The virtual path of an id, or nullopt when the tree has none.
    using PathOf = std::function<std::optional<std::string>(AssetId)>;

    /// @param source serves the source files by id. Must outlive this provider.
    /// @param pathOf names each id's file, whose extension picks its kind. Called
    ///        from workers, so it must be safe to call from several at once.
    CookingProvider(const AssetProvider &source, PathOf pathOf);

    /// @return The cooked blob; UnknownAssetId when the id has no path or its
    ///         file is no registered kind; UnsupportedEncoding, with the reason
    ///         logged, when the kind's cook step refuses the file.
    [[nodiscard]] std::expected<std::vector<std::byte>, AssetError> Open(AssetId id) const override;

    [[nodiscard]] std::expected<AssetId, AssetError> Resolve(std::string_view vpath) const override;

  private:
    PathOf _pathOf;
    const AssetProvider *_source;
};

} // namespace Assisi::Core
