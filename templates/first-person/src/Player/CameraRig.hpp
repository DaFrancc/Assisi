/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CameraRig.hpp
/// @brief Where a character's view camera sits, and which camera this machine
///        looks through.
///
/// Part of the first-person template: copy it into your game's `src/Player/`
/// with the rest of the folder (the template's README says what else to copy).
/// The view camera is the entity a
/// character's ViewRig names, usually a child of the character. Look tilts it
/// up and down, and LookAhead draws it following the mouse every frame;
/// EyeHeight keeps it at the character's eye height, which drops as it
/// crouches; PlayerView makes this machine's player's camera the one drawn.
///
/// To lay the rig out differently, such as a head entity between character
/// and camera for view effects to move, point ViewRig at whichever entity
/// should tilt and sit at eye height.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/ECS/Entity.hpp>

namespace Game
{

/// @brief Names a character's view camera. On the character.
ACOMP()
struct ViewRig
{
    /// The entity that tilts with the look and sits at eye height: in a
    /// blueprint, the name of the camera's entity.
    AFIELD() Assisi::ECS::Entity camera = Assisi::ECS::NullEntity;
};

/// @brief Keeps every character's view camera at the character's eye height.
///
/// After the step, which eases the eye toward its stance's height; predicted,
/// so a client stepping its character again puts the camera where each step
/// left it.
ASYSTEM(PostFixedUpdate, name = "EyeHeight", predicted)
void EyeHeightSystem(Assisi::App::SystemContext &ctx);

/// @brief Makes this machine's player's view camera the active one, and every
/// other character's inactive.
///
/// `activeWorldOnly`: only the world being shown and played chooses a camera
/// to look through.
ASYSTEM(Update, name = "PlayerView", activeWorldOnly)
void PlayerViewSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
