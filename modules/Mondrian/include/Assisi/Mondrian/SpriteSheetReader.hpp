/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SpriteSheetReader.hpp
/// @brief Where a sprite sheet comes from, installed per executable.
///
/// A shipped game reads the sheets the cook wrote into its package; the editor
/// compiles a source file in process and reads the blob it just produced. Both
/// end in ReadCookedSpriteSheet, as a theme's two readers end in
/// ReadCookedTheme, so there is one path from bytes to a sheet.

#include <Assisi/Mondrian/Sprite.hpp>
#include <Assisi/Mondrian/UiConfig.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>

namespace Assisi::Core
{
class AssetProvider;
}

namespace Assisi::Mondrian
{

/// @brief Why a sprite sheet did not arrive, or did not read.
enum class SpriteSheetReadError : uint8_t
{
    Missing,    ///< No sheet at that path.
    Unreadable, ///< It exists and its bytes could not be read.
    Invalid,    ///< The bytes were read and are not a usable sheet.
    NoReader,   ///< No reader is installed.
    Count_
};

[[nodiscard]] std::string_view ToString(SpriteSheetReadError error) noexcept;

/// @brief A sheet that did not load, and which one.
struct SpriteSheetLoadError
{
    std::string vpath;
    SpriteSheetReadError error = SpriteSheetReadError::Missing;
};

using SpriteSheetReader =
    std::function<std::expected<SpriteSheetDocument, SpriteSheetReadError>(std::string_view vpath)>;

/// @brief Install the reader every sheet load goes through, returning the one
/// it replaces. Install at startup, before the first sheet loads: the reader is
/// read without a lock.
SpriteSheetReader SetSpriteSheetReader(SpriteSheetReader reader);

/// @brief The sheet at @p vpath, through the installed reader.
[[nodiscard]] std::expected<SpriteSheetDocument, SpriteSheetReadError> LoadSpriteSheet(std::string_view vpath);

/// @brief Every sheet @p config lists, in its order. The first that does not
/// load is the error, and no sheets are returned with it.
[[nodiscard]] std::expected<SpriteSheets, SpriteSheetLoadError> LoadSpriteSheets(const UiConfig &config);

/// @brief The reader for sheets cooked into @p provider.
[[nodiscard]] std::expected<SpriteSheetDocument, SpriteSheetReadError> ReadCookedSpriteSheet(
    const Core::AssetProvider &provider, std::string_view vpath);

} // namespace Assisi::Mondrian
