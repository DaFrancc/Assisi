/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AnimationSystems.hpp
/// @brief The system that plays every AnimationPlayer.

#include <Assisi/Core/Reflect/Annotations.hpp>

namespace Assisi::App
{

struct SystemContext;

/// @brief Plays every AnimationPlayer's clip or blend space on its SkinnedMesh
///        for the frame.
///
/// Update, so animations play only while the world simulates, and code that
/// adjusts joints on top runs after, in PostUpdate. A player whose mesh, clip,
/// space or any of the space's clips has not loaded yet waits. One whose clips
/// name joints its mesh lacks plays the rest and warns once, until its
/// animation or mesh changes.
ASYSTEM(Update, name = "AnimationPlayers") void AnimationPlayerSystem(SystemContext &ctx);

} // namespace Assisi::App
