/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/CookingProvider.hpp>

#include <Assisi/Core/AssetKind.hpp>
#include <Assisi/Core/Logger.hpp>

#include <format>
#include <utility>

namespace Assisi::Core
{

namespace
{

/// Other source files as the provider serves them, by the id their path has.
class ProviderCookContext final : public AssetCookContext
{
  public:
    ProviderCookContext(const AssetProvider &source, const CookingProvider::TextOf &kindOf, std::string path)
        : _path(std::move(path)), _kindOf(&kindOf), _source(&source)
    {
    }

    [[nodiscard]] std::string_view Path() const override { return _path; }

    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> Read(std::string_view vpath) const override
    {
        const std::optional<AssetId> id = IdFor(vpath);
        if (!id)
        {
            return std::unexpected(std::format("\"{}\" is not a file in the asset tree", vpath));
        }
        std::expected<std::vector<std::byte>, AssetError> bytes = _source->Open(*id);
        if (!bytes)
        {
            return std::unexpected(std::format("\"{}\" can't be read: {}", vpath, Describe(bytes.error())));
        }
        return std::move(*bytes);
    }

    [[nodiscard]] std::optional<AssetId> IdFor(std::string_view vpath) const override
    {
        const std::expected<AssetId, AssetError> id = _source->Resolve(vpath);
        return id ? std::optional<AssetId>{*id} : std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> KindNameOf(AssetId id) const override { return (*_kindOf)(id); }

    void Report(std::string message) const override { Log::Warn("CookingProvider: '{}': {}", _path, message); }

  private:
    std::string _path;
    const CookingProvider::TextOf *_kindOf;
    const AssetProvider *_source;
};

} // namespace

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
    const ProviderCookContext context{*_source, _kindOf, *path};
    std::expected<std::vector<std::byte>, AssetError> cooked = CookAssetBytes(*kind, *source, context);
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
