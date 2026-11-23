/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PlayerSystems.hpp
/// @brief The systems that drive a player's character from the player's
///        controls, the same on every machine.
///
/// PlayerGather fills in this machine's player's controls from the keyboard and
/// mouse. CharacterCommand turns any player's controls into where its character
/// faces and what it is asked to do, whoever filled them in: this machine, or a
/// host for a remote player. LocalPlayer says which character is this machine's
/// player, and PlayerCamera looks through its camera.

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/ECS/SceneFwd.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::App
{

struct SystemContext;

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

/// @brief @p facing, a character's rotation, turned left by @p radians about
/// world up.
///
/// About world up rather than the character's own axis: composing onto its own
/// would let pitch and roll leak in, and it would end up tipped over after
/// enough looking around.
[[nodiscard]] glm::quat Turned(const glm::quat &facing, float radians);

/// @brief @p eye, a camera's rotation on its character, tilted up by
/// @p radians and kept within kMaxPitchDegrees of level.
[[nodiscard]] glm::quat Tilted(const glm::quat &eye, float radians);

/// @brief Marks this machine's player: with no network session every Player,
/// and in one the Player this machine controls. Gives each a PlayerInput, and
/// takes both away from a character that stops being this machine's, so its
/// last controls do not walk it on by themselves.
///
/// Reads the world's LocalClient. Until a session has said who this machine is,
/// nothing is its player.
ASYSTEM(FixedUpdate, name = "LocalPlayer", before = "CharacterCommand")
void LocalPlayerSystem(SystemContext &ctx);

/// @brief Fills in the controls of this machine's player from the keyboard and
/// mouse.
///
/// Movement and held buttons are what the devices say now; presses and turning
/// are added up until a fixed step uses them. Turns only while the cursor is
/// captured, and never captures it: whoever owns the session owns the cursor,
/// because only it knows when the player has asked for the mouse back.
///
/// **PreUpdate, not FixedUpdate.** A key press is an edge that exists for
/// exactly one frame, and the mouse moves a whole frame's worth between polls.
/// Read from a fixed step, a frame that runs no step loses them and one that
/// runs three counts them three times. PreUpdate runs once a frame, before the
/// frame's steps, so those steps act on what this frame gathered.
///
/// The camera of this machine's player is drawn turned as far as what is
/// gathered here, every frame, before any step has used it (ActiveSceneCamera).
///
/// `activeWorldOnly`, and it does nothing without devices: a headless host has
/// none.
ASYSTEM(PreUpdate, name = "PlayerGather", activeWorldOnly)
void PlayerGatherSystem(SystemContext &ctx);

/// @brief Turns each player's controls into its character's facing and
/// CharacterIntent, then clears what the step used.
///
/// Turning goes on the character, about world up; looking up and down goes on
/// the camera parented to it, so the body turns and the head tilts. Movement is
/// taken in the character's frame after it turns. A press asks for one jump
/// however long it is held; Crouch and Sprint are asked for while they are
/// held. A character without PlayerInput is left alone, so
/// an AI keeps writing its own intent.
///
/// Every machine runs it on every character with controls, which is what makes
/// a host move a remote player exactly as that player's own machine does.
///
/// `predicted`: a client stepping its character again turns and moves it from
/// the controls it recorded for each tick.
ASYSTEM(FixedUpdate, name = "CharacterCommand", predicted)
void CharacterCommandSystem(SystemContext &ctx);

/// @brief Clears what a step used of every player's controls in @p scene: the
/// turning and the presses, which add up until a step uses them. What is held
/// stays, since it is what the devices say now.
///
/// Not a system: StepWorld calls it once every system of the step has run,
/// live or replayed, so every system reads the same turning and presses in any
/// order, none of them has to be the one that clears them, and no level or
/// blueprint has to name it. CharacterCommand also clears what it uses, as it
/// uses it, so where it runs, a system after it in the step sees no turning or
/// presses.
void ClearUsedControls(ECS::Scene &scene);

/// @brief Makes the camera of this machine's player the active one, and every
/// other player's camera inactive.
///
/// Every player's blueprint carries a camera, and a networked game holds every
/// player's; each machine looks through its own.
ASYSTEM(Update, name = "PlayerCamera", activeWorldOnly)
void PlayerCameraSystem(SystemContext &ctx);

/// @brief Takes the cursor when the level is up, so a first-person level is
/// playable the moment it finishes loading.
///
/// A level names this, or the blueprint of a player it places, to say "I am
/// played with the mouse". Capturing it unconditionally would be wrong for a
/// level that is a menu, and nothing in the engine can tell those apart.
///
/// `Loaded` rather than `Begin`: taking the cursor is the last thing before a
/// player has control, and holding it while the level is still streaming means
/// grabbing the pointer during the pause.
///
/// `activeWorldOnly`, and it null-checks input regardless: a headless host has
/// no window, so this is inert there even when a level names it.
ASYSTEM(Loaded, name = "CaptureCursor", activeWorldOnly)
void CaptureCursorSystem(SystemContext &ctx);

/// @brief A click in the window takes the cursor back after a menu has handed
/// it out.
///
/// The running half of CaptureCursor, and a level that names one wants both.
/// Escape is not read here: it opens the pause menu, which is what hands the
/// cursor over, and the menu gives it back on Resume. Two things listening for
/// the same key would fight over it. A click while a screen takes input is the
/// screen's, so it never recaptures the pointer.
///
/// `activeWorldOnly`, and it null-checks input regardless: a headless host has
/// no window, so this is inert there even when a level names it.
ASYSTEM(Update, name = "CursorToggle", activeWorldOnly)
void CursorToggleSystem(SystemContext &ctx);

} // namespace Assisi::App
