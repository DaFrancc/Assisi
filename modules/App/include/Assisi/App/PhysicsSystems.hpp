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

#include <Assisi/Core/Reflect/Annotations.hpp>

#include <string_view>

namespace Assisi::App
{

struct SystemContext;

/// @brief Action names CharacterInputSystem polls.
///
/// Constants rather than string literals at the call site, so a level's
/// `input.actions` table and the code that reads it cannot drift apart by a typo
/// that simply reads as the key doing nothing.
///
/// `string_view`, not `const char *`: the lookups take one, so a pointer would
/// have its length measured again on every call, for every character, every step.
inline constexpr std::string_view kActionMoveForward  = "MoveForward";
inline constexpr std::string_view kActionMoveBackward = "MoveBackward";
inline constexpr std::string_view kActionMoveLeft     = "MoveLeft";
inline constexpr std::string_view kActionMoveRight    = "MoveRight";
inline constexpr std::string_view kActionJump         = "Jump";
inline constexpr std::string_view kActionCrouch       = "Crouch";

/// @brief Puts every Camera parented to a character at that character's eye
/// height, from the step that just ran.
///
/// PostFixedUpdate, so the height is the one the step left in its
/// CharacterState. The camera's Transform is written inside the step like the
/// character's own, so the render blend moves both across the same two steps:
/// a crouch is smooth at any refresh rate, and a crouch in the air — feet up,
/// eye height down by as much — never shows one without the other.
ASYSTEM(PostFixedUpdate, name = "CharacterEye") void CharacterEyeSystem(SystemContext &ctx);

/// @brief Drives every Physics::Character in the active world from the keyboard,
/// through its CharacterIntent.
///
/// **A stand-in, and deliberately a crude one.** It steers every character in the
/// world at once, because nothing yet says which one the player is — possession
/// is what replaces this, and when it lands this system is what gets deleted. It
/// exists so a level can be walked around before that.
///
/// A server writes `CharacterIntent` from replicated input commands instead of
/// polling devices, which is why the intent is a component rather than being
/// read from a device inside the controller: the two paths meet at the same
/// three fields.
///
/// Movement is in world axes — forward is −Z — since there is no camera to be
/// relative to yet.
///
/// @par Requirements
/// **Update, not FixedUpdate.** Devices are sampled once per frame, and a key
/// *press* is an edge that exists for exactly that frame. Read from a fixed step
/// it is both lost and duplicated: a frame fast enough to run no step never sees
/// the press at all, and a frame slow enough to run three sees the same one three
/// times. Once per frame is the only reading that matches how the edge is
/// produced. The controller consumes the intent on every step, which is where the
/// fixed rate belongs.
///
/// `activeWorldOnly`, and it null-checks the input anyway: a headless host has no
/// devices, so this is inert there even when a level names it.
ASYSTEM(Update, name = "CharacterInput", activeWorldOnly)
void CharacterInputSystem(SystemContext &ctx);

/// @brief Degrees of turn per pixel of mouse movement.
///
/// A constant rather than a Character field because look sensitivity is a
/// *player's* preference, not a property of a character — it belongs in the
/// options a person sets once, and authoring it per character would make every
/// NPC carry a number that means nothing to it. Here until there is somewhere
/// better to put it.
inline constexpr float kLookDegreesPerPixel = 0.1f;

/// @brief How far up or down a character may look.
///
/// Short of straight up, because at exactly 90 the forward direction becomes
/// parallel to the up axis and the view matrix's basis collapses — the image
/// rolls or vanishes at the moment the player is least able to explain why.
inline constexpr float kMaxPitchDegrees = 89.f;

/// @brief Turns the mouse into where a character is looking, and holds the
/// cursor while it does.
///
/// Yaw goes on the character, pitch on the camera parented to it — the standard
/// split, and the reason the controller writes position only and never rotates
/// the capsule: facing is gameplay's, and this is gameplay.
///
/// The camera is found by looking for a Camera whose Parent is the character, so
/// a character without one simply turns on the spot. Nothing here needs the
/// camera to be the *active* one; a character that is not being viewed still
/// faces where it was told to.
///
/// **A stand-in, like CharacterInputSystem, and it goes when possession lands.**
///
/// Looks only while the cursor is already captured, and never captures it itself.
/// Taking it would mean deciding when the player may have it back, which is the
/// host's business — an editor hands it over on Play and returns it on Escape or
/// F8, and a game build would simply hold it. A system that grabbed the cursor
/// every step would undo any release a step after it happened.
///
/// @par Requirements
/// **Update, before CharacterInput** — per frame, for the same reason as that
/// system and more so: `MouseDelta` is the movement accumulated since the last
/// poll, which is a whole frame's worth. Consuming it from a fixed step applies
/// one frame of movement once, twice or not at all depending on how many steps
/// the accumulator happens to run, so the same flick of the wrist turns a
/// different amount at 144 Hz than at 60. Once per frame turns exactly once.
///
/// Sensitivity is degrees per *pixel* and is deliberately not scaled by dt: the
/// delta is already a distance moved, not a rate, and multiplying it by a
/// timestep would make a fast frame turn less for the same hand movement.
///
/// `activeWorldOnly`, and it null-checks the input regardless: a headless host
/// has no mouse, and a server reads look from replicated commands instead.
ASYSTEM(Update, name = "CharacterLook", before = "CharacterInput", activeWorldOnly)
void CharacterLookSystem(SystemContext &ctx);

} // namespace Assisi::App
