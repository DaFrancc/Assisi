/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Sprint.hpp
/// @brief Holding the Sprint input action makes the player run faster.
///
/// A game action and the system that reads it: the example of a speed a game
/// sets on its characters through CharacterIntent::speedScale, which works
/// whatever locomotion preset the character uses and is predicted with no code
/// of its own.

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

namespace Game
{

inline constexpr Assisi::App::PlayerAction Sprint = Assisi::App::GameAction(0);

inline constexpr Assisi::App::PlayerActionName kPlayerActions[] = {
    {"Sprint", Sprint},
};
ASSISI_PLAYER_ACTIONS(kPlayerActions);

/// How much faster a player runs while Sprint is held.
inline constexpr float kSprintSpeedScale = 1.6f;

/// Sets each player's speed scale from whether Sprint is held.
ASYSTEM(FixedUpdate, name = "Sprint", before = "CharacterCommand")
void SprintSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
