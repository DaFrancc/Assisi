/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file UiConfig.hpp
/// @brief The project's UI settings: which files are string tables, themes and
/// sprite sheets, and how strictly the cook treats the text screens show.
///
/// A `config/ui.json` document. The cook reads it to check screens and tables;
/// the game reads it to know which tables, themes and sheets to load.

#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

#include <string_view>
#include <vector>

namespace Assisi::Mondrian
{

/// @brief Where the UI settings live.
inline constexpr std::string_view kUiConfigPath = "config/ui.json";

AASSET()
struct UiConfig
{
    /// Every string table the project has. A `.csv` listed nowhere here is not
    /// a string table, whatever it holds.
    AFIELD() std::vector<Assisi::Core::AssetPath> stringTables;

    /// Every theme the project has. The first is the one the game starts in,
    /// in its first mode, and every screen is checked against all of them.
    AFIELD() std::vector<Assisi::Core::AssetPath> themes;

    /// Every sprite sheet the project has. The game uploads each one's texture
    /// at start, and a sprite a screen or a theme names must be in one of them.
    AFIELD() std::vector<Assisi::Core::AssetPath> spriteSheets;

    /// Whether text a player reads must come from a table. On, literal text on
    /// such a field fails the cook, except on a screen marked debug-only.
    AFIELD() bool requireStringKeys = false;

    /// Whether a table may leave a key's text empty. Off, an empty cell fails
    /// the cook: it is almost always a string nobody has written yet.
    AFIELD() bool allowEmptyStrings = false;
};

} // namespace Assisi::Mondrian
