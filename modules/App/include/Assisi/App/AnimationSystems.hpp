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
/// What it plays is the request in the player's AnimationPlayback: what its
/// Animator picked, or, with no Animator that has its file, the player's own
/// fields. It only reads the AnimationPlayer and SkinnedMesh, and writes their
/// AnimationPlayback and SkinnedPose, so playing sends nothing over the network.
///
/// Does nothing on a host whose context does not animate, such as a dedicated
/// server: a headless host evaluates no animation.
///
/// Update, so animations play only while the world simulates, and code that
/// adjusts joints on top runs after it, in Update with after = "AnimationPlayers"
/// or in Ui. A player whose mesh, clip,
/// space or any of the space's clips has not loaded yet waits. One whose clips
/// name joints its mesh lacks plays the rest and warns once, until its
/// animation or mesh changes.
ASYSTEM(Update, name = "AnimationPlayers") void AnimationPlayerSystem(SystemContext &ctx);

/// @brief Runs every Animator's `.sgl` file for the frame, telling its
///        AnimationPlayer what to play.
///
/// Before AnimationPlayers in the same Update, so what a transition chose
/// plays this frame. Off, like AnimationPlayers, on a host that does not
/// animate. An Animator waits while its file loads; one whose file
/// is changed while the game runs carries on in the new one from where it
/// was, and keeps the old one while the new one loads or if it won't cook.
ASYSTEM(Update, name = "Animators", before = "AnimationPlayers") void AnimatorSystem(SystemContext &ctx);

} // namespace Assisi::App
