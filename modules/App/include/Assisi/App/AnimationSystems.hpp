/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AnimationSystems.hpp
/// @brief The system that plays every AnimationPlayer.

#include <Assisi/Core/Reflect/Annotations.hpp>

namespace Assisi::App
{

struct SystemContext;

/// @brief Plays every AnimationPlayer's clip on its SkinnedMesh for the frame.
///
/// Update, so clips play only while the world simulates, and code that adjusts
/// joints on top of a clip runs after it, in PostUpdate. A player whose mesh or
/// clip has not loaded yet waits. One whose clip names joints its mesh lacks
/// plays the rest and warns once, until its clip or mesh changes.
ASYSTEM(Update, name = "AnimationPlayers") void AnimationPlayerSystem(SystemContext &ctx);

} // namespace Assisi::App
