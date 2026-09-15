/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AppConfig.hpp
/// @brief What the game ships with: window, clear colour, physics rate, and the
///        diagnostics retention counts.
///
/// Everything here is fixed at build time and is not the player's to change.
/// What the player changes lives in OptionsConfig, under the writable user root
/// — including the window size, whose shipped default is the one below.

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Core/Reflect/AssetDocument.hpp>
#include <Assisi/Core/TrivialString.hpp>
#include <Assisi/Math/Color.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace Assisi::App
{

/// @brief The game config document, as it sits on disk.
///
/// Ordered widest field first so the struct carries no interior padding; the
/// inline string, alone in being two-byte aligned, goes after the scalars
/// rather than between them.
AASSET()
struct AppConfig
{
    AFIELD() double physicsHz = 60.0;

    AFIELD() Assisi::Math::Color4 clearColor{0.15f, 0.15f, 0.18f, 1.f};

    /// @brief The window size a fresh install starts at.
    ///
    /// A default, not the answer: a player's chosen resolution is stored under
    /// the user root and applied over this, so a patch that changes what ships
    /// does not undo what they picked.
    AFIELD() int32_t width  = 1280;
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

    /// @brief The OS window title.
    ///
    /// Sixty-four bytes rather than thirty-two: a title is product branding, it
    /// runs long ("Studio — Game Name (Early Access)"), and an inline string
    /// truncates on assignment without telling anyone. The wider capacity puts
    /// the cut out of reach instead of leaving a silent one halfway through.
    AFIELD() Assisi::Core::TrivialString<64> title{"Assisi Game"};

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
