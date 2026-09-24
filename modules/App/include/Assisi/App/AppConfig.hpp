/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AppConfig.hpp
/// @brief What the game ships with: window, clear colour, physics rate, the
///        scene it boots into, and the diagnostics retention counts.
///
/// Everything here is fixed at build time and is not the player's to change.
/// What the player changes lives in OptionsConfig, under the writable user root
/// — including the window size, whose shipped default is the one below.

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Core/Reflect/AssetDocument.hpp>
#include <Assisi/Core/TrivialString.hpp>
#include <Assisi/Math/Color.hpp>
#include <Assisi/Mondrian/ScaleMatch.hpp>
#include <Assisi/Window/InputEvent.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace Assisi::App
{

/// @brief When a world starts simulating once its content is committed.
///
/// Both one-shot phases run either way; this decides only whether the clock
/// starts at the first or waits for the second.
AENUM()
enum class SimulateFrom : std::uint8_t
{
    /// Simulate as soon as the world works as data, while its meshes and
    /// materials are still streaming in behind placeholders. What a game with no
    /// loading screen wants: the level is playable the moment it is coherent.
    Begin,

    /// Hold the simulation still until every asset has settled. What a game with
    /// a loading screen wants — the screen comes down and the world is already
    /// whole, with nothing popping in behind it.
    Loaded,

    Count
};

/// @brief The game config document, as it sits on disk.
///
/// Ordered widest field first so the struct carries no interior padding; the
/// inline strings, alone in being two-byte aligned, go after the scalars rather
/// than between them.
AASSET()
struct AppConfig
{
    AFIELD() double physicsHz = 60.0;

    /// @brief The longest gap between presses that still continues a double
    /// tap (or a longer run), in seconds: the game's standard.
    AFIELD() double multiTapSeconds = Window::kDefaultMultiTapSeconds;

    AFIELD() Assisi::Math::Color4<Assisi::Math::ColorSpace::Srgb> clearColor { 0.15f, 0.15f, 0.18f, 1.f };

    /// @brief The window size a fresh install starts at.
    ///
    /// A default, not the answer: a player's chosen resolution is stored under
    /// the user root and applied over this, so a patch that changes what ships
    /// does not undo what they picked.
    AFIELD() int32_t width = 1280;
    AFIELD() int32_t height = 720;

    /// @brief How many past runs' logs to keep.
    ///
    /// Each launch writes its own timestamped file and the oldest are pruned, so
    /// a player who relaunches after a crash does not overwrite the run that
    /// explains it. Five covers "it happened a few launches ago" without letting
    /// the directory grow without bound.
    ///
    /// A total including the run in progress, whose file is passed to the
    /// pruner as protected and so is never a deletion candidate. 0 is valid and
    /// means "keep no history"; this run's log still survives.
    AFIELD() uint32_t keepLogs = 5;

    /// @brief How many past crash reports to keep.
    ///
    /// Same policy as keepLogs, and the names share a launch stamp so a report
    /// and the log from the same run pair up. Counted slightly differently in
    /// practice: pruning runs at startup, before this run's report exists, so a
    /// run that crashes leaves keepDumps + 1 behind until the next launch.
    AFIELD() uint32_t keepDumps = 5;

    /// @brief Whether the game starts simulating before its assets have settled.
    ///
    /// Begin by default: a game that says nothing about loading gets a world that
    /// runs as soon as it is coherent, which is what it had before this existed.
    /// A game that draws a loading screen sets Loaded, so the simulation is still
    /// behind the screen and nothing pops in once it lifts.
    ///
    /// Only the clock waits. Both one-shot phases run either way, so the logic
    /// that takes the screen down belongs in a Loaded system whichever this says.
    AFIELD() SimulateFrom simulateFrom = SimulateFrom::Begin;

    /// @brief Whether a player's own tap interval, from their options, replaces
    /// multiTapSeconds. Off holds every player to the standard.
    AFIELD() bool playerSetsMultiTap = true;

    /// @brief Whether the pointer moving over a UI node focuses it, so the keys
    /// act on what is pointed at. Off leaves focus where the keys or a click put it.
    AFIELD() bool uiHoverFocuses = false;

    /// @brief Which side of the screen a UI pixel is measured against: the
    /// shorter side, so text reads the same on any shape of screen, or the width
    /// or height for a game designed for one orientation.
    AFIELD() Mondrian::ScaleMatch uiScaleMatch = Mondrian::ScaleMatch::ShorterSide;

    /// @brief The OS window title.
    ///
    /// Sixty-four bytes rather than thirty-two: a title is product branding, it
    /// runs long ("Studio — Game Name (Early Access)"), and an inline string
    /// truncates on assignment without telling anyone. The wider capacity puts
    /// the cut out of reach instead of leaving a silent one halfway through.
    AFIELD() Assisi::Core::TrivialString<64> title { "Assisi Game" };

    /// @brief The scene the game opens at boot — a virtual path or an asset GUID.
    ///
    /// Empty by default, and empty is refused out loud rather than defaulted to
    /// some level: a game that boots content nobody named is a game whose
    /// configuration is not being read. The game takes no level argument, so this
    /// is the whole of what decides what a player sees.
    ///
    /// Sixty-four bytes for the same reason the title has them — a path under
    /// levels/ with a descriptive name runs long, and an inline string truncates
    /// on assignment in silence. A cut one does not resolve, so it surfaces as
    /// the startup refusal naming the truncated text rather than as a game that
    /// opens the wrong scene.
    AFIELD() Assisi::Core::TrivialString<64> startupScene {};

    /// @brief Parse @p text as a game config document.
    ///
    /// Anything the document does not mention keeps its default, and a document
    /// that will not parse at all yields defaults entire — a file someone
    /// hand-edited into nonsense costs the settings, never the launch.
    [[nodiscard]] static AppConfig FromJsonText(std::string_view text);

    /// @brief Read the shipped config from the asset root.
    [[nodiscard]] static AppConfig Load(std::string_view assetPath = "config/game.json");
};

} // namespace Assisi::App
