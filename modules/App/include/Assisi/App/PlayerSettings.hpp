/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PlayerSettings.hpp
/// @brief The player's options as game code reaches them: read, change, apply, save.

#include <Assisi/App/OptionsConfig.hpp>

namespace Assisi::Audio
{
class Mixer;
} // namespace Assisi::Audio

namespace Assisi::App
{

/// @brief What a settings screen needs: the options, and a way to make a change
/// take effect and to keep it. Systems reach it through `ctx.settings`.
///
/// Changing Options() alone changes nothing the player sees or hears; the Apply
/// calls do that, and Save writes options.json so the change outlives the run.
/// They are separate so a screen can try a value without keeping it, and keep
/// several changes with one save.
class PlayerSettings
{
public:
    /// @param mixer Null when the process has no audio; ApplyAudio then does nothing.
    PlayerSettings(OptionsConfig &options, Audio::Mixer *mixer) noexcept;

    [[nodiscard]] OptionsConfig &Options() noexcept { return _options; }

    /// @brief Sets every bus the options name to the player's volume. A name the
    /// game has no bus for is skipped, and kept in the options.
    void ApplyAudio();

    /// @brief Writes the options to options.json.
    void Save() const;

private:
    OptionsConfig &_options;
    Audio::Mixer *_mixer;
};

} // namespace Assisi::App
