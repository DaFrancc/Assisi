/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PlayerAnimation.hpp
/// @brief Animates every player's body from what its character does: the
///        book's example of feeding an animator from CharacterState.
///
/// It runs on every machine, for every player, so each machine animates the
/// other players itself from the character state the host sends; nothing about
/// animation crosses the network. For files written with `speed`, `airborne`,
/// `crouched`, `jump`, `land` and `interact` params, as `animation/Player.sgl`
/// is.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/SceneFwd.hpp>

namespace Game
{

/// Update, before the Animators read the params.
ASYSTEM(Update, name = "PlayerAnimation", before = "Animators")
void PlayerAnimationSystem(Assisi::App::SystemContext &ctx);

/// Plays @p player's gesture: fires the `interact` trigger of every animator on
/// an entity parented to it.
void PlayGesture(Assisi::ECS::Scene &scene, Assisi::ECS::Entity player);

} // namespace Game
