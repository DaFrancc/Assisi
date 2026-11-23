/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Movement.hpp
/// @brief Turns a player's controls into what its character is asked to do:
///        where to go, and whether to jump, crouch or sprint.
///
/// Part of the first-person template: copy it into your game's `src/Player/`
/// with the rest of the folder (the template's README says what else to copy).
/// The character controller does the moving; this only fills in its
/// CharacterIntent each fixed step. A full move
/// asks for the controller's `walkSpeed`. Holding Walk shortens the move to
/// Movement::walkMoveScale of it, for keys, which can only ask for a full move;
/// a stick pushed part way asks for less on its own.
///
/// After Look, so the move goes the way the character faces this step.
/// `predicted`, so a client stepping its character again asks the same of it.
///
/// Speeds, jump height and the rest of how a character moves are on its
/// movement component (ArcadeMovement or SourceMovement) in the blueprint.
/// Change what the buttons ask for here: a dash, a slide, a different crouch.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

namespace Game
{

/// @brief How a player's controls move its character. On the character.
ACOMP()
struct Movement
{
    /// The fraction of a full move kept while Walk is held.
    AFIELD(min = 0.0, max = 1.0) float walkMoveScale = 0.4f;
};

/// @brief Fills in every character's intent from its controls.
ASYSTEM(FixedUpdate, name = "Movement", predicted, after = "Look")
void MovementSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
