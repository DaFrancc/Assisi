/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Look.hpp
/// @brief Looking around: the character turns left and right, its view camera
///        tilts up and down, and the view follows the mouse every frame.
///
/// Part of the first-person template: copy it into your game's `src/Player/`
/// with the rest of the folder (the template's README says what else to copy).
/// Turning goes on the character itself,
/// about world up, so it walks the way it faces. Tilting goes on the camera
/// its ViewRig names, so the body stays upright. Both happen in the fixed
/// step, the same on every machine, which is what makes a host turn a remote
/// player exactly as that player's own machine does; `predicted`, so a client
/// stepping its character again turns it from the controls it recorded for
/// each step.
///
/// The fixed step runs at a set rate, so a view turned only there would turn
/// in steps on a faster screen. LookAhead draws this machine's player's view
/// turned on by the mouse movement no step has used yet, every frame, through
/// the camera's CameraDrawPose. Only the drawing turns; the step still does
/// the real turning.
///
/// To change how far the view tilts, set Look::maxPitchDegrees on the
/// character in its blueprint. For a third-person camera, point ViewRig at a
/// boom the camera hangs from instead of the camera itself.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

namespace Game
{

/// @brief The furthest up or down a view may look.
///
/// Short of straight up, because at exactly 90 the forward direction becomes
/// parallel to the up axis and the view's basis collapses: the image rolls or
/// vanishes at the moment the player is least able to explain why.
inline constexpr float kMaxLookPitchDegrees = 89.f;

/// @brief How a character looks around. On the character.
ACOMP()
struct Look
{
    /// How far up or down the view may tilt.
    AFIELD(min = 0.0, max = 89.0) float maxPitchDegrees = kMaxLookPitchDegrees;
};

/// @brief Turns and tilts every character with controls, Look and a ViewRig by
/// the look its controls ask for.
ASYSTEM(FixedUpdate, name = "Look", predicted, after = "LocalPlayer")
void LookSystem(Assisi::App::SystemContext &ctx);

/// @brief Draws this machine's player's view turned as far as the mouse has
/// moved, every frame.
///
/// Update, after the frame's fixed steps have used and cleared what they
/// could: the turning left in the controls is what no step has used yet. A
/// character that stops being this machine's player has the pose taken off
/// its camera again.
ASYSTEM(Update, name = "LookAhead", activeWorldOnly)
void LookAheadSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
