/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PlayerSystems.hpp
/// @brief What the engine gives every game about its players: whose character
///        is this machine's, its controls from the devices, and the cursor.
///
/// LocalPlayer says which character is this machine's player. PlayerGather
/// fills in its controls from the keyboard and mouse, and StepWorld clears
/// what each step used of them (ClearUsedControls). What a game does with the
/// controls, turning, moving and which camera to look through, is the game's
/// own: the first-person template in templates/first-person is one way to do
/// it.

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

/// @brief Marks this machine's player: with no network session every Player,
/// and in one the Player this machine controls. Gives each a PlayerInput, and
/// takes both away from a character that stops being this machine's, so its
/// last controls do not walk it on by themselves.
///
/// Reads the world's LocalClient. Until a session has said who this machine is,
/// nothing is its player. A system that reads the controls runs
/// `after = "LocalPlayer"`, so a character gains its controls before the step
/// that first reads them.
ASYSTEM(FixedUpdate, name = "LocalPlayer")
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
/// What is gathered here and no step has used yet is still in the controls
/// when a frame draws, so a game can draw its view turned on by it every frame
/// (Runtime::CameraDrawPose), as the first-person template does.
///
/// `activeWorldOnly`, and it does nothing without devices: a headless host has
/// none.
ASYSTEM(PreUpdate, name = "PlayerGather", activeWorldOnly)
void PlayerGatherSystem(SystemContext &ctx);

/// @brief Clears what a step used of every player's controls in @p scene: the
/// turning and the presses, which add up until a step uses them. What is held
/// stays, since it is what the devices say now.
///
/// Not a system: StepWorld calls it once every system of the step has run,
/// live or replayed, so every system reads the same turning and presses in any
/// order, none of them has to be the one that clears them, and no level or
/// blueprint has to name it.
void ClearUsedControls(ECS::Scene &scene);

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
