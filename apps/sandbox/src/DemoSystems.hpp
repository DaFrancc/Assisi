/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file DemoSystems.hpp
/// @brief The sandbox's game logic, declared where a level can name it.
///
/// A minimum of real game logic, so per-world system binding is observable in
/// the editor rather than only in the headless tests: Play should visibly do
/// something, Pause should visibly stop it, and a second resident world should
/// spin on its own.
///
/// All three are *stateless* — everything they touch lives in components — which
/// is the shape a system installed into several worlds must have
/// (docs/world-system-binding-design-notes.md §1).
///
/// They live in a header rather than in main.cpp because ASYSTEM is read by
/// reflectgen, and a declaration in a scanned header *is* the registration.
/// Linking this module puts them in the catalog; a level names the ones it wants.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

namespace Sandbox
{

/// Spins every non-physics entity about Y. Physics-driven entities are excluded
/// so this never fights Jolt for the same pose.
ASYSTEM(Update, name = "SpinDemo") void SpinDemoSystem(Assisi::App::SystemContext &ctx);

/// Reports the space bar. `activeWorldOnly`, so with two worlds simulating only
/// the active one reacts — the "one InputContext, N worlds" rule made visible.
/// Also the demo of SystemContext's nullable input.
ASYSTEM(Update, name = "InputDemo", after = SpinDemo, activeWorldOnly)
void InputDemoSystem(Assisi::App::SystemContext &ctx);

} // namespace Sandbox
