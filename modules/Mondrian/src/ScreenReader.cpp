/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/ScreenReader.hpp>

#include <Assisi/Mondrian/ScreenBlob.hpp>

#include <Assisi/Core/AssetProvider.hpp>
#include <Assisi/Core/Logger.hpp>

#include <cstddef>
#include <utility>
#include <vector>

namespace Assisi::Mondrian
{
namespace
{

ScreenReader &InstalledReader()
{
    static ScreenReader reader;
    return reader;
}

ScreenIdReader &InstalledIdReader()
{
    static ScreenIdReader reader;
    return reader;
}

/// The cooked screen @p id names in @p provider, called @p name in what is
/// logged.
std::expected<ScreenDocument, ScreenReadError> ReadCooked(const Core::AssetProvider &provider, Core::AssetId id,
                                                          std::string_view name)
{
    const std::expected<std::vector<std::byte>, Core::AssetError> bytes = provider.Open(id);
    if (!bytes)
    {
        return std::unexpected(bytes.error() == Core::AssetErrorCode::UnknownAssetId ? ScreenReadError::Missing
                                                                                     : ScreenReadError::Unreadable);
    }
    std::expected<ScreenDocument, CookedScreenError> document = ReadCookedScreen(*bytes);
    if (!document)
    {
        Core::Log::Error("Mondrian: '{}' is not a usable screen ({}).", name, ToString(document.error()));
        return std::unexpected(ScreenReadError::Invalid);
    }
    return std::move(*document);
}

} // namespace

std::string_view ToString(ScreenReadError error) noexcept
{
    switch (error)
    {
    case ScreenReadError::Missing:
        return "no screen at that path";
    case ScreenReadError::Unreadable:
        return "the screen could not be read";
    case ScreenReadError::Invalid:
        return "the screen is not usable";
    case ScreenReadError::NoReader:
        return "no screen reader is installed";
    case ScreenReadError::Count_:
        break;
    }
    return "unknown";
}

ScreenReader SetScreenReader(ScreenReader reader)
{
    return std::exchange(InstalledReader(), std::move(reader));
}

std::expected<ScreenDocument, ScreenReadError> LoadScreenDocument(std::string_view vpath)
{
    const ScreenReader &reader = InstalledReader();
    if (!reader)
    {
        // No fallback to reading files: a game that quietly opened a loose
        // screen would hide a package that is missing one.
        Core::Log::Error("Mondrian: no screen reader is installed to load '{}'.", vpath);
        return std::unexpected(ScreenReadError::NoReader);
    }
    return reader(vpath);
}

ScreenIdReader SetScreenIdReader(ScreenIdReader reader)
{
    return std::exchange(InstalledIdReader(), std::move(reader));
}

std::expected<ScreenDocument, ScreenReadError> LoadScreenDocument(Core::AssetId id)
{
    const ScreenIdReader &reader = InstalledIdReader();
    if (!reader)
    {
        Core::Log::Error("Mondrian: no screen reader is installed to load the screen {}.", id.ToString());
        return std::unexpected(ScreenReadError::NoReader);
    }
    return reader(id);
}

std::expected<ScreenDocument, ScreenReadError> ReadCookedScreen(const Core::AssetProvider &provider,
                                                                std::string_view vpath)
{
    const std::expected<Core::AssetId, Core::AssetError> id = provider.Resolve(vpath);
    if (!id)
    {
        return std::unexpected(id.error() == Core::AssetErrorCode::UnknownAssetId ? ScreenReadError::Missing
                                                                                  : ScreenReadError::Unreadable);
    }
    return ReadCooked(provider, *id, vpath);
}

std::expected<ScreenDocument, ScreenReadError> ReadCookedScreen(const Core::AssetProvider &provider, Core::AssetId id)
{
    return ReadCooked(provider, id, id.ToString());
}

} // namespace Assisi::Mondrian
