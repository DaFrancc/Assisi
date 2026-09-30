/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/CookingProvider.hpp>

#include <Assisi/Core/AssetKind.hpp>
#include <Assisi/Core/Logger.hpp>

#include <utility>

namespace Assisi::Core
{

CookingProvider::CookingProvider(const AssetProvider &source, TextOf pathOf, TextOf kindOf)
    : _pathOf(std::move(pathOf)), _kindOf(std::move(kindOf)), _source(&source)
{
}

std::expected<std::vector<std::byte>, AssetError> CookingProvider::Open(AssetId id) const
{
    const std::optional<std::string> path = _pathOf(id);
    if (!path)
    {
        return std::unexpected(AssetErrorCode::UnknownAssetId);
    }
    const std::optional<std::string> kindName = _kindOf(id);
    if (!kindName)
    {
        Log::Warn("CookingProvider: '{}' has no kind in its sidecar.", *path);
        return std::unexpected(AssetErrorCode::UnknownAssetId);
    }
    const AssetKindRegistry &registry = AssetKindRegistry::Instance();
    const AssetKind *kind = registry.FindByName(*kindName);
    // The engine's own kinds have no load function here: they are read by their
    // own loaders, never through this provider.
    if (kind == nullptr || !kind->load)
    {
        return std::unexpected(AssetErrorCode::UnknownAssetId);
    }
    if (!registry.Reads(kind->id, ExtensionOf(*path)))
    {
        Log::Warn("CookingProvider: '{}' is used as {}, which does not read its format.", *path, kind->name);
        return std::unexpected(AssetErrorCode::UnknownAssetId);
    }

    const std::expected<std::vector<std::byte>, AssetError> source = _source->Open(id);
    if (!source)
    {
        return std::unexpected(source.error());
    }
    std::expected<std::vector<std::byte>, AssetError> cooked = CookAssetBytes(*kind, *source);
    if (!cooked)
    {
        // Logged here, where the path is known; the caller has only the id.
        Log::Warn("CookingProvider: '{}' does not cook as {}: {}", *path, kind->name, Describe(cooked.error()));
        return std::unexpected(cooked.error());
    }
    return std::move(*cooked);
}

std::expected<AssetId, AssetError> CookingProvider::Resolve(std::string_view vpath) const
{
    return _source->Resolve(vpath);
}

} // namespace Assisi::Core
