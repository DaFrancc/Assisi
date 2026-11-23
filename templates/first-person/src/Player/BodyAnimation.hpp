/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file BodyAnimation.hpp
/// @brief Animates every character's body from what the character does.
///
/// Part of the first-person template: copy it into your game's `src/Player/`
/// with the rest of the folder (the template's README says what else to copy).
/// A body is any entity with an Animator
/// parented to a character. Each frame its animator's params are set from the
/// character's CharacterState: `speed` (m/s across the ground, eased),
/// `airborne`, `crouched`, and the triggers `jump` and `land`. The template's
/// animator file, `animation/Player.sgl`, uses those params; add more here for
/// anything else yours needs. Its `interact` trigger is the game's to fire, for
/// a gesture such as pressing a button: call PlayGesture.
///
/// Runs on every machine for every character, so each machine animates the
/// others itself from the character state the host sends; nothing about
/// animation crosses the network.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/SceneFwd.hpp>

namespace Game
{

/// @brief Sets every character's body's animator params. Before the Animators
/// read them.
ASYSTEM(Update, name = "BodyAnimation", before = "Animators")
void BodyAnimationSystem(Assisi::App::SystemContext &ctx);

/// @brief Plays @p character's gesture: fires the `interact` trigger of every
/// animator on an entity parented to it.
///
/// Call it on every machine, from an event the host sent, so everyone sees the
/// gesture; nothing about animation crosses the network.
void PlayGesture(Assisi::ECS::Scene &scene, Assisi::ECS::Entity character);

} // namespace Game
