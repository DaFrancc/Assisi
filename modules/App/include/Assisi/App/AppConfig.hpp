/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AppConfig.hpp
/// @brief Engine configuration loaded from assets/game.json.

#include <Assisi/Math/GLM.hpp>

#include <cstdint>
#include <string>

namespace Assisi::App
{

struct AppConfig
{
    std::string title      = "Assisi Game";
    int32_t     width      = 1280;
    int32_t     height     = 720;
    glm::vec4   clearColor = {0.15f, 0.15f, 0.18f, 1.f};
    double      physicsHz  = 60.0;

    /// @brief Run with no window, renderer, input, or debug UI — the dedicated
    /// server mode (`"headless": true` at the root of game.json). A command-line
    /// flag can also set it; the two OR together, so a config that omits the key
    /// never turns a `--server` invocation back into a windowed one.
    bool headless = false;

    /// @brief How many past runs' logs to keep (`diagnostics.keepLogs`).
    ///
    /// Each launch writes its own timestamped file and the oldest are pruned,
    /// so a player who relaunches after a crash no longer overwrites the run
    /// that explains it. Five covers "it happened a few launches ago" without
    /// letting the directory grow without bound.
    ///
    /// A total including the run in progress, whose file is passed to the
    /// pruner as protected and so is never a deletion candidate. 0 is valid and
    /// means "keep no history"; this run's log still survives.
    uint32_t keepLogs = 5;

    /// @brief How many past crash reports to keep (`diagnostics.keepDumps`).
    ///
    /// Same policy as keepLogs, and the names share a launch stamp so a report
    /// and the log from the same run pair up. Counted slightly differently in
    /// practice: pruning runs at startup, before this run's report exists, so a
    /// run that crashes leaves keepDumps + 1 behind until the next launch.
    uint32_t keepDumps = 5;

    /// @brief Reads assets/game.json via the asset system.
    /// Falls back to defaults if the file is missing or malformed.
    static AppConfig LoadFromJson();
};

} // namespace Assisi::App
