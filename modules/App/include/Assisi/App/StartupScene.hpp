/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file StartupScene.hpp
/// @brief Turning what the shipped config names into a level path the game can
///        open, or saying exactly why it cannot.
///
/// A game takes no level argument, so the one string in AppConfig::startupScene
/// is all that decides what a player sees. Every way it can fail is a different
/// repair, which is why the failure is an enum rather than a bool: an empty
/// field is a config nobody filled in, an unknown GUID is a scene that moved out
/// from under its id, and an unresolvable path is a scene that was never staged.

#include <Assisi/Core/AssetDatabase.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace Assisi::App
{

/// @brief Why the configured startup scene is not a level this install can open.
enum class StartupSceneError : std::uint8_t
{
    Unnamed,     ///< The config names no scene at all.
    UnknownGuid, ///< A well-formed asset id the index does not hold.
    Missing,     ///< A virtual path with no readable file behind it.
};

/// @brief One line saying what is wrong, for the log that refuses the launch.
[[nodiscard]] std::string_view Describe(StartupSceneError error);

/// @brief Resolve @p named — a virtual path or an asset GUID — to the virtual
/// path of the level to boot.
///
/// A GUID is tried first and never falls through to the path branch: text that
/// parses as an id and is not in @p database is reported as a missing id, not as
/// a missing file, because the two are repaired in different places. Anything
/// that is not an id is a virtual path, and comes back unchanged when the asset
/// system can read it.
[[nodiscard]] std::expected<std::string, StartupSceneError> ResolveStartupScene(
    std::string_view named, const Core::AssetDatabase &database);

} // namespace Assisi::App
