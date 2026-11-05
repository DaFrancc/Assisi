/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Hud.hpp
/// @brief The heads-up display: a player's speed in the bottom right.
///
/// What it shows, and where, is in ui/Hud.amdn, bound to the player's
/// CharacterState. What is here is what a file cannot say: which entity is a
/// player, and that each one gets a HUD of its own.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

#include <string_view>

namespace Game
{

/// Where the screen is authored.
inline constexpr std::string_view kHudScreenPath = "ui/Hud.amdn";

/// @brief The HUD a player's character shows, kept on the character so each
/// player reaches its own.
ACOMP(transient)
struct PlayerHud
{
    Assisi::App::ScreenId screen;
};

/// Pushed by a HUD whose character was destroyed. PlayerHud went with the
/// character, so the screen says which one it is.
AEVENT()
struct HudOrphaned
{
    Assisi::App::ScreenId screen;
};

/// Gives a character a HUD of its own, shown and about it, when it becomes
/// this machine's player, and takes it away when control moves elsewhere. Does
/// nothing in a frame where neither happened.
ASYSTEM(Update, name = "Hud")
void HudSystem(Assisi::App::SystemContext &ctx);

/// Removes each HUD whose character was destroyed. In PreUpdate because a
/// paused world still runs it: its Update does not run, and its events end with
/// the frame.
ASYSTEM(PreUpdate, name = "HudOrphans")
void HudOrphanSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
