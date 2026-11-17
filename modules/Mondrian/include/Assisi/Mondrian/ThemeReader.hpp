/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ThemeReader.hpp
/// @brief Where a theme comes from, installed per executable.
///
/// A shipped game reads the themes the cook wrote into its package; the editor
/// compiles a source file in process and reads the blob it just produced. Both
/// end in ReadCookedTheme, as a screen's two readers end in ReadCookedScreen,
/// so there is one path from bytes to a theme.

#include <Assisi/Mondrian/Theme.hpp>
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

/// @brief Why a theme did not arrive, or did not read.
enum class ThemeReadError : uint8_t
{
    Missing,    ///< No theme at that path.
    Unreadable, ///< It exists and its bytes could not be read.
    Invalid,    ///< The bytes were read and are not a usable theme.
    NoReader,   ///< No reader is installed.
    Count_
};

[[nodiscard]] std::string_view ToString(ThemeReadError error) noexcept;

/// @brief A theme that did not load, and which one.
struct ThemeLoadError
{
    std::string vpath;
    ThemeReadError error = ThemeReadError::Missing;
};

using ThemeReader = std::function<std::expected<ThemeDocument, ThemeReadError>(std::string_view vpath)>;

/// @brief Install the reader every theme load goes through, returning the one
/// it replaces. Install at startup, before the first theme loads: the reader is
/// read without a lock.
ThemeReader SetThemeReader(ThemeReader reader);

/// @brief The theme at @p vpath, through the installed reader.
[[nodiscard]] std::expected<ThemeDocument, ThemeReadError> LoadTheme(std::string_view vpath);

/// @brief Every theme @p config lists, in its order. The first that does not
/// load is the error, and no themes are returned with it.
[[nodiscard]] std::expected<Themes, ThemeLoadError> LoadThemes(const UiConfig &config);

/// @brief The reader for themes cooked into @p provider.
[[nodiscard]] std::expected<ThemeDocument, ThemeReadError> ReadCookedTheme(const Core::AssetProvider &provider,
                                                                           std::string_view vpath);

} // namespace Assisi::Mondrian
