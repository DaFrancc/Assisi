/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PlayerActions.hpp
/// @brief The buttons a player holds beyond the engine's Jump, Crouch and
///        Sprint.
///
/// Part of the first-person template: copy it into your game's `src/Player/`
/// with the rest of the folder (the template's README says what else to copy).
/// Each action reads the input action of the same name in
/// your `config/input.json` and travels with the player's controls to the host
/// like the engine's own, so add the line the template's README gives. Add
/// your own actions here, one GameAction slot each.

#include <Assisi/App/PlayerComponents.hpp>

namespace Game::Actions
{

/// Held to walk rather than jog on keys, which can only ask for a full move. A
/// stick pushed part way walks without it.
inline constexpr Assisi::App::PlayerAction Walk = Assisi::App::GameAction(0);

inline constexpr Assisi::App::PlayerActionName kPlayerActions[] = {
    {"Walk", Walk},
};
ASSISI_PLAYER_ACTIONS(kPlayerActions);

} // namespace Game::Actions
