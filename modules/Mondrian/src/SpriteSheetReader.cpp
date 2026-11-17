/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/SpriteSheetReader.hpp>

#include <Assisi/Core/AssetProvider.hpp>
#include <Assisi/Core/Logger.hpp>

#include <cstddef>
#include <utility>
#include <vector>

namespace Assisi::Mondrian
{
namespace
{

SpriteSheetReader &InstalledReader()
{
    static SpriteSheetReader reader;
    return reader;
}

} // namespace

std::string_view ToString(SpriteSheetReadError error) noexcept
{
    switch (error)
    {
    case SpriteSheetReadError::Missing:
        return "no sprite sheet at that path";
    case SpriteSheetReadError::Unreadable:
        return "the sprite sheet could not be read";
    case SpriteSheetReadError::Invalid:
        return "the sprite sheet is not usable";
    case SpriteSheetReadError::NoReader:
        return "no sprite sheet reader is installed";
    case SpriteSheetReadError::Count_:
        break;
    }
    return "unknown";
}

SpriteSheetReader SetSpriteSheetReader(SpriteSheetReader reader)
{
    return std::exchange(InstalledReader(), std::move(reader));
}

std::expected<SpriteSheetDocument, SpriteSheetReadError> LoadSpriteSheet(std::string_view vpath)
{
    const SpriteSheetReader &reader = InstalledReader();
    if (!reader)
    {
        // No fallback to reading files: a game that quietly opened a loose
        // sheet would hide a package that is missing one.
        Core::Log::Error("Mondrian: no sprite sheet reader is installed to load '{}'.", vpath);
        return std::unexpected(SpriteSheetReadError::NoReader);
    }
    return reader(vpath);
}

std::expected<SpriteSheets, SpriteSheetLoadError> LoadSpriteSheets(const UiConfig &config)
{
    SpriteSheets sheets;
    for (const Core::AssetPath &path : config.spriteSheets)
    {
        const std::string_view vpath = path.View();
        std::expected<SpriteSheetDocument, SpriteSheetReadError> sheet = LoadSpriteSheet(vpath);
        if (!sheet)
        {
            return std::unexpected(SpriteSheetLoadError{.vpath = std::string{vpath}, .error = sheet.error()});
        }
        sheets.listed.push_back(ListedSheet{.vpath = std::string{vpath}, .document = std::move(*sheet)});
    }
    return sheets;
}

std::expected<SpriteSheetDocument, SpriteSheetReadError> ReadCookedSpriteSheet(const Core::AssetProvider &provider,
                                                                               std::string_view vpath)
{
    const std::expected<Core::AssetId, Core::AssetError> id = provider.Resolve(vpath);
    if (!id)
    {
        return std::unexpected(id.error() == Core::AssetErrorCode::UnknownAssetId ? SpriteSheetReadError::Missing
                                                                                  : SpriteSheetReadError::Unreadable);
    }
    const std::expected<std::vector<std::byte>, Core::AssetError> bytes = provider.Open(*id);
    if (!bytes)
    {
        return std::unexpected(SpriteSheetReadError::Unreadable);
    }
    std::expected<SpriteSheetDocument, CookedSpriteSheetError> sheet = ReadCookedSpriteSheet(*bytes);
    if (!sheet)
    {
        Core::Log::Error("Mondrian: '{}' is not a usable sprite sheet ({}).", vpath, ToString(sheet.error()));
        return std::unexpected(SpriteSheetReadError::Invalid);
    }
    return std::move(*sheet);
}

} // namespace Assisi::Mondrian
