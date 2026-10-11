/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PhysicsSystems.hpp
/// @brief Ready-made systems that turn the physics components in
///        Assisi::Physics into behaviour.
///
/// These live here rather than in Assisi::Physics because a system needs a
/// SystemContext, and App is the layer that owns one — Physics deliberately does
/// not depend on it. They are plain functions, never installed automatically: a
/// file names the ones it needs, so a world
/// that has no use for one does not run it.
///
/// What drives a player's character from the controls is in PlayerSystems.hpp.

#include <Assisi/Core/Reflect/Annotations.hpp>

namespace Assisi::App
{

struct SystemContext;

/// @brief Puts every Camera parented to a character at that character's eye
/// height, from the step that just ran.
///
/// PostFixedUpdate, so the height is the one the step left in its
/// CharacterState. The camera's Transform is written inside the step like the
/// character's own, so the render blend moves both across the same two steps:
/// a crouch is smooth at any refresh rate, and a crouch in the air — feet up,
/// eye height down by as much — never shows one without the other.
ASYSTEM(PostFixedUpdate, name = "CharacterEye") void CharacterEyeSystem(SystemContext &ctx);

} // namespace Assisi::App
