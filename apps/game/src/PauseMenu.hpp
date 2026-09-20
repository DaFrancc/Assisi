/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PauseMenu.hpp
/// @brief The pause menu: Paused, Resume, Quit, built from the node API.
///
/// Escape shows it, Escape or Resume hides it, Quit ends the game. While it is
/// up it takes the pointer and the keys, so nothing reaches gameplay, and the
/// world stops because a stacked screen is a screen the player is not playing
/// through.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

namespace Assisi::Mondrian
{
class Screen;
class Ui;
} // namespace Assisi::Mondrian

namespace Game
{

/// The name the pause menu is created under, which is how a system with
/// nowhere to keep a pointer finds it again.
inline constexpr std::string_view kPauseScreenName = "Pause";

/// What Resume pushes. Quit pushes App::QuitRequested, which the host answers.
struct ResumeClicked
{
};

/// @brief Builds the pause menu into @p ui and returns it, hidden.
Assisi::Mondrian::Screen *BuildPauseMenu(Assisi::Mondrian::Ui &ui);

/// Shows the pause menu on Escape and hides it again on Resume.
///
/// It builds the menu the first time it runs and finds it by name after that:
/// a system is a free function with nowhere to keep a pointer. Escape closing
/// the menu is not read here — the UI has the keys while a screen takes input,
/// so Back pops the screen before the game ever sees the key.
///
/// `activeWorldOnly`, and it null-checks the UI and input: a headless host has
/// neither.
ASYSTEM(Update, name = "PauseMenu", activeWorldOnly)
void PauseMenuSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
