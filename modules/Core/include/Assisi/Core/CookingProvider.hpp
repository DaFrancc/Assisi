/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CookingProvider.hpp
/// @brief Cooked blobs made on demand from source files, for every host that
///        works on the source tree rather than a package.
///
/// A game reads blobs the cook wrote into its package. The editor and tools
/// have the source files, so this runs the same cook, in memory, when a blob is
/// asked for: the file's sidecar names its kind, and the kind's cook step turns
/// the source into the payload. An AssetStore reads through either one the same
/// way, so there is one load path whichever host it is.
///
/// Only kinds a module registered are served. The engine's own kinds keep their
/// own source readers.

#include <Assisi/Core/AssetProvider.hpp>

#include <functional>
#include <optional>
#include <string>

namespace Assisi::Core
{

class CookingProvider final : public AssetProvider
{
  public:
    /// @brief Something about an id, or nullopt when there is none.
    using TextOf = std::function<std::optional<std::string>(AssetId)>;

    /// @param source serves the source files by id. Must outlive this provider.
    /// @param pathOf names each id's file. @param kindOf names the kind each id's
    ///        sidecar gives it. Both are called from workers, so they must be
    ///        safe to call from several at once.
    CookingProvider(const AssetProvider &source, TextOf pathOf, TextOf kindOf);

    /// @return The cooked blob. UnknownAssetId when the id has no path, no kind,
    ///         a kind no module registered, or a kind that does not read the
    ///         file's format; UnsupportedEncoding, with the reason logged, when
    ///         the kind's cook step refuses the file.
    [[nodiscard]] std::expected<std::vector<std::byte>, AssetError> Open(AssetId id) const override;

    [[nodiscard]] std::expected<AssetId, AssetError> Resolve(std::string_view vpath) const override;

  private:
    TextOf _pathOf;
    TextOf _kindOf;
    const AssetProvider *_source;
};

} // namespace Assisi::Core
