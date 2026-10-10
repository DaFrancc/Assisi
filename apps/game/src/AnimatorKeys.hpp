/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AnimatorKeys.hpp
/// @brief Drives every Animator from the keyboard and mouse, to watch an
///        animation file's states without gameplay behind them.
///
/// For files written with `speed`, `grounded`, `jump` and `aiming` params, as
/// the book's character is. Hold W to walk, add Shift to run, press Space to
/// jump and hold it to stay in the air, hold the right mouse button to aim.
/// The character runs in place.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

namespace Game
{

/// Update, before the Animators read the params; activeWorldOnly, so only the
/// world being played answers the keys.
ASYSTEM(Update, name = "AnimatorKeys", before = "Animators", activeWorldOnly)
void AnimatorKeysSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
