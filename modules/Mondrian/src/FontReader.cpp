/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/FontReader.hpp>

#include <Assisi/Core/AssetProvider.hpp>
#include <Assisi/Core/Logger.hpp>

#include <utility>

namespace Assisi::Mondrian
{
namespace
{

FontReader &InstalledReader()
{
    static FontReader reader;
    return reader;
}

} // namespace

std::string_view ToString(FontLoadError error) noexcept
{
    switch (error)
    {
    case FontLoadError::Missing:
        return "no font at that path";
    case FontLoadError::Unreadable:
        return "the font could not be read";
    case FontLoadError::Invalid:
        return "the font is not usable";
    case FontLoadError::NoReader:
        return "no font reader is installed";
    case FontLoadError::Count:
        break;
    }
    return "unknown";
}

FontReader SetFontReader(FontReader reader)
{
    return std::exchange(InstalledReader(), std::move(reader));
}

std::expected<Font, FontLoadError> LoadFont(std::string_view vpath)
{
    const FontReader &reader = InstalledReader();
    if (!reader)
    {
        // No fallback to reading files: a game that quietly opened a loose font
        // would hide a package that is missing one.
        Core::Log::Error("Mondrian: no font reader is installed to load '{}'.", vpath);
        return std::unexpected(FontLoadError::NoReader);
    }
    return reader(vpath);
}

std::expected<Font, FontLoadError> ReadCookedFont(const Core::AssetProvider &provider, std::string_view vpath)
{
    const std::expected<Core::AssetId, Core::AssetError> id = provider.Resolve(vpath);
    if (!id)
    {
        return std::unexpected(id.error() == Core::AssetError::UnknownAssetId ? FontLoadError::Missing
                                                                              : FontLoadError::Unreadable);
    }
    const std::expected<std::vector<std::byte>, Core::AssetError> bytes = provider.Open(*id);
    if (!bytes)
    {
        return std::unexpected(FontLoadError::Unreadable);
    }
    std::expected<Font, CookedFontError> font = ReadCookedFont(*bytes);
    if (!font)
    {
        Core::Log::Error("Mondrian: '{}' is not a usable font ({}).", vpath, ToString(font.error()));
        return std::unexpected(FontLoadError::Invalid);
    }
    return std::move(*font);
}

} // namespace Assisi::Mondrian
