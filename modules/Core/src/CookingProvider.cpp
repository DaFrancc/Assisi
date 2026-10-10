/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/CookingProvider.hpp>

#include <Assisi/Core/AssetKind.hpp>
#include <Assisi/Core/Logger.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <mutex>
#include <span>
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

    /// Every file read through this context, for a change to any of them to
    /// cook the asset again.
    [[nodiscard]] const std::vector<std::string> &ReadPaths() const { return _read; }

    [[nodiscard]] std::string_view Path() const override { return _path; }

    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> Read(std::string_view vpath) const override
    {
        _read.emplace_back(vpath);
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
    mutable std::vector<std::string> _read;
    const CookingProvider::TextOf *_kindOf;
    const AssetProvider *_source;
};

/// @p path, every file @p context read, and every file @p kind's cook step
/// says @p source depends on, such as a model it reads without the context.
std::vector<std::string> WatchedPaths(const std::string &path, const AssetKind &kind,
                                      std::span<const std::byte> source, const ProviderCookContext &context)
{
    std::vector<std::string> paths{path};
    paths.insert(paths.end(), context.ReadPaths().begin(), context.ReadPaths().end());
    const AssetCookStep *step = AssetKindRegistry::Instance().CookStepFor(kind.id);
    if (step != nullptr && step->dependencies)
    {
        std::vector<std::string> dependencies = step->dependencies(source, context);
        paths.insert(paths.end(), dependencies.begin(), dependencies.end());
    }
    return paths;
}

} // namespace

CookingProvider::CookingProvider(const AssetProvider &source, TextOf pathOf, TextOf kindOf, StampOf stampOf)
    : _pathOf(std::move(pathOf)), _kindOf(std::move(kindOf)), _stampOf(std::move(stampOf)), _source(&source)
{
}

void CookingProvider::Watch(AssetId id, std::vector<std::string> paths) const
{
    if (!_stampOf)
    {
        return;
    }
    std::vector<WatchedFile> files;
    for (std::string &path : paths)
    {
        const std::optional<std::int64_t> stamp = _stampOf(path);
        files.push_back(WatchedFile{.path = std::move(path), .stamp = stamp});
    }
    const std::lock_guard lock{_watchedMutex};
    _watched[id] = std::move(files);
}

std::vector<AssetId> CookingProvider::ChangedAssets()
{
    std::vector<AssetId> changed;
    const std::lock_guard lock{_watchedMutex};
    for (std::unordered_map<AssetId, std::vector<WatchedFile>>::iterator entry = _watched.begin();
         entry != _watched.end();)
    {
        const bool moved = std::ranges::any_of(
            entry->second, [this](const WatchedFile &file) { return _stampOf(file.path) != file.stamp; });
        if (!moved)
        {
            ++entry;
            continue;
        }
        changed.push_back(entry->first);
        entry = _watched.erase(entry);
    }
    return changed;
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
    // Watched whether or not it cooked, so fixing a refused file loads it again.
    Watch(id, WatchedPaths(*path, *kind, *source, context));
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
