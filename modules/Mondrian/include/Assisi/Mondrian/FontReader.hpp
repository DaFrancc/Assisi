/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file FontReader.hpp
/// @brief Where fonts come from, installed per executable.
///
/// A shipped game reads the fonts the cook wrote into its package; the editor
/// works on the source tree and rasterises a description when asked. Both answer
/// the same question, so the UI asks this seam and each executable installs the
/// reader that matches the files it has. Only the installed one is linked, which
/// is what keeps the rasteriser out of the game.

#include <Assisi/Mondrian/Font.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <string_view>

namespace Assisi::Core
{
class AssetProvider;
}

namespace Assisi::Mondrian
{

/// @brief Why a font did not load.
enum class FontLoadError : uint8_t
{
    Missing,    ///< No font at that path.
    Unreadable, ///< The font exists and its bytes could not be read.
    Invalid,    ///< The bytes were read and are not a usable font.
    NoReader,   ///< No reader is installed.
    Count
};

[[nodiscard]] std::string_view ToString(FontLoadError error) noexcept;

using FontReader = std::function<std::expected<Font, FontLoadError>(std::string_view vpath)>;

/// @brief Install the reader every font load goes through, returning the one it
///        replaces. Install at startup, before the UI loads a font: the reader is
///        read without a lock.
FontReader SetFontReader(FontReader reader);

/// @brief The font at @p vpath (a `.afont`), through the installed reader.
[[nodiscard]] std::expected<Font, FontLoadError> LoadFont(std::string_view vpath);

/// @brief The reader for fonts cooked into @p provider.
[[nodiscard]] std::expected<Font, FontLoadError> ReadCookedFont(const Core::AssetProvider &provider,
                                                               std::string_view vpath);

} // namespace Assisi::Mondrian
