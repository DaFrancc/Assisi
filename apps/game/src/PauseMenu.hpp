/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PauseMenu.hpp
/// @brief The pause menu: Paused, Resume, Quit, written as a screen file.
///
/// Escape shows it, Escape or Resume hides it, Quit ends the game. While it is
/// up it takes the pointer and the keys, so nothing reaches gameplay, and the
/// world stops because a screen that hides what is beneath is a screen the
/// player is not playing through.
///
/// What the menu looks like and what its buttons do is in ui/Pause.amdn. What
/// is here is the two things a file cannot say: when to show it, and the key
/// that does.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

#include <string_view>

namespace Game
{

/// Where the screen is authored, which is also how a system with nowhere to
/// keep a pointer finds it again.
inline constexpr std::string_view kPauseScreenPath = "ui/Pause.amdn";

/// Gives the world a pause menu, and asks for the system that opens it.
///
/// Named by the BaseGameplay blueprint rather than by each level: a level that
/// places that blueprint is a level that has a pause menu, and a main menu
/// leaves it out. The screen is destroyed with the world it paused.
///
/// The system the menu needs is named in the file rather than here, so a screen
/// that grows a settings page asks for what that needs without this changing.
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
/// Declared by the screen file, so a level that shows the menu installs this
/// without naming it. `activeWorldOnly`: only the world on screen is paused by
/// the player's Escape.
ASYSTEM(Update, name = "PauseMenu", activeWorldOnly)
void PauseMenuSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
