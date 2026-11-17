/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/ThemeReader.hpp>

#include <Assisi/Core/AssetProvider.hpp>
#include <Assisi/Core/Logger.hpp>

#include <cstddef>
#include <utility>
#include <vector>

namespace Assisi::Mondrian
{
namespace
{

ThemeReader &InstalledReader()
{
    static ThemeReader reader;
    return reader;
}

} // namespace

std::string_view ToString(ThemeReadError error) noexcept
{
    switch (error)
    {
    case ThemeReadError::Missing:
        return "no theme at that path";
    case ThemeReadError::Unreadable:
        return "the theme could not be read";
    case ThemeReadError::Invalid:
        return "the theme is not usable";
    case ThemeReadError::NoReader:
        return "no theme reader is installed";
    case ThemeReadError::Count_:
        break;
    }
    return "unknown";
}

ThemeReader SetThemeReader(ThemeReader reader)
{
    return std::exchange(InstalledReader(), std::move(reader));
}

std::expected<ThemeDocument, ThemeReadError> LoadTheme(std::string_view vpath)
{
    const ThemeReader &reader = InstalledReader();
    if (!reader)
    {
        // No fallback to reading files: a game that quietly opened a loose
        // theme would hide a package that is missing one.
        Core::Log::Error("Mondrian: no theme reader is installed to load '{}'.", vpath);
        return std::unexpected(ThemeReadError::NoReader);
    }
    return reader(vpath);
}

std::expected<Themes, ThemeLoadError> LoadThemes(const UiConfig &config)
{
    Themes themes;
    for (const Core::AssetPath &path : config.themes)
    {
        const std::string_view vpath = path.View();
        std::expected<ThemeDocument, ThemeReadError> theme = LoadTheme(vpath);
        if (!theme)
        {
            return std::unexpected(ThemeLoadError{.vpath = std::string{vpath}, .error = theme.error()});
        }
        themes.listed.push_back(ListedTheme{.vpath = std::string{vpath}, .document = std::move(*theme)});
    }
    return themes;
}

std::expected<ThemeDocument, ThemeReadError> ReadCookedTheme(const Core::AssetProvider &provider,
                                                             std::string_view vpath)
{
    const std::expected<Core::AssetId, Core::AssetError> id = provider.Resolve(vpath);
    if (!id)
    {
        return std::unexpected(id.error() == Core::AssetErrorCode::UnknownAssetId ? ThemeReadError::Missing
                                                                                  : ThemeReadError::Unreadable);
    }
    const std::expected<std::vector<std::byte>, Core::AssetError> bytes = provider.Open(*id);
    if (!bytes)
    {
        return std::unexpected(ThemeReadError::Unreadable);
    }
    std::expected<ThemeDocument, CookedThemeError> theme = ReadCookedTheme(*bytes);
    if (!theme)
    {
        Core::Log::Error("Mondrian: '{}' is not a usable theme ({}).", vpath, ToString(theme.error()));
        return std::unexpected(ThemeReadError::Invalid);
    }
    return std::move(*theme);
}

} // namespace Assisi::Mondrian
