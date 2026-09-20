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

#include <memory>
#include <string_view>

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

/// @brief The pause menu, hidden, owned by the caller.
///
/// Resume and Back close it and need nothing installed. Quit pushes
/// App::QuitRequested, which the host answers — that one leaves the UI, so it
/// goes through an event and a system reads it.
[[nodiscard]] std::unique_ptr<Assisi::Mondrian::Screen> BuildPauseMenu(Assisi::Mondrian::Ui &ui);

/// Gives the world a pause menu, and asks for the system that opens it.
///
/// Named by the BaseGameplay blueprint rather than by each level: a level that
/// places that blueprint is a level that has a pause menu, and a main menu
/// leaves it out. The screen is destroyed with the world it paused.
///
/// Not `activeWorldOnly`: a world builds its own screen as it loads, whether or
/// not it is the one on screen when it does.
ASYSTEM(Loaded, name = "PauseMenuScreen")
void PauseMenuScreenSystem(Assisi::App::SystemContext &ctx);

/// Shows the pause menu on Escape.
///
/// Only the opening: closing is carried on the Resume button and answered by
/// Back, neither of which reaches the world. Escape is not read for closing
/// either — the UI has the keys while the menu is up, so Back pops the screen
/// before the game ever sees that press.
///
/// Declared by the screen above, so a level that shows the menu installs this
/// without naming it. `activeWorldOnly`: only the world on screen is paused by
/// the player's Escape.
ASYSTEM(Update, name = "PauseMenu", activeWorldOnly)
void PauseMenuSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
