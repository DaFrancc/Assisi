/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file DemoSystems.hpp
/// @brief The game's own logic, declared where a level can name it.
///
/// A minimum of real game logic, so per-world system binding is observable in
/// the editor rather than only in the headless tests: Play should visibly do
/// something, Pause should visibly stop it, and a second resident world should
/// spin on its own.
///
/// All three are *stateless* — everything they touch lives in components — which
/// is the shape a system installed into several worlds must have.
///
/// They live in a header rather than in main.cpp because ASYSTEM is read by
/// reflectgen, and a declaration in a scanned header *is* the registration.
/// Linking this module puts them in the catalog; a level names the ones it wants.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

namespace Game
{

/// Spins every non-physics entity about Y. Physics-driven entities are excluded
/// so this never fights Jolt for the same pose.
ASYSTEM(Update, name = "SpinDemo") void SpinDemoSystem(Assisi::App::SystemContext &ctx);

/// Reports the space bar. `activeWorldOnly`, so with two worlds simulating only
/// the active one reacts — the "one InputContext, N worlds" rule made visible.
/// Also the demo of SystemContext's nullable input.
ASYSTEM(Update, name = "InputDemo", activeWorldOnly)
void InputDemoSystem(Assisi::App::SystemContext &ctx);

/// Drops a Bouncer on F3. Its own system rather than a branch inside InputDemo,
/// because a level that wants this does not thereby want the space-bar logger:
/// a level's systems list names behaviour one piece at a time.
///
/// `activeWorldOnly`, and it null-checks input regardless: a headless host has
/// no devices, so this is inert there even when a level names it.
///
/// **Spawns into the world it runs in.** On a client that world is a mirror and
/// the entity is local-only — it replicates nowhere and the server never learns
/// of it. Watching a Bouncer reach a client means pressing this on the
/// authority.
ASYSTEM(Update, name = "BouncerSpawn", after = SpinDemo, activeWorldOnly)
void BouncerSpawnSystem(Assisi::App::SystemContext &ctx);

/// Takes the cursor when the level is up, so a first-person level is playable
/// the moment it finishes loading.
///
/// A level names this to say "I am played with the mouse". The alternative was
/// the game capturing the cursor unconditionally, which is wrong for a level
/// that is a menu — and nothing in the engine can tell those apart.
///
/// `Loaded` rather than `Begin`: taking the cursor is the last thing before a
/// player has control, and holding it while the level is still streaming means
/// grabbing the pointer during the pause.
///
/// `activeWorldOnly`, and it null-checks input regardless: a headless host has
/// no window, so this is inert there even when a level names it.
ASYSTEM(Loaded, name = "CaptureCursor", activeWorldOnly)
void CaptureCursorSystem(Assisi::App::SystemContext &ctx);

/// Escape hands the cursor back; a click in the window takes it again.
///
/// The running half of CaptureCursor, and a level that names one wants both.
/// Which key is listened for depends on which state the cursor is in, so the two
/// can never fight: while it is held, only Escape is read, and while it is free,
/// only the click. Escape does not quit — that is what closing the window is
/// for, and quitting is the one response a player cannot undo.
///
/// `activeWorldOnly`, and it null-checks input regardless: a headless host has
/// no window, so this is inert there even when a level names it.
ASYSTEM(Update, name = "CursorToggle", activeWorldOnly)
void CursorToggleSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
