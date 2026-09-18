/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file InputSetup.hpp
/// @brief Filling an ActionMap from the two places bindings come from.
///
/// Every windowed host needs the same two layers in the same order, and a host
/// that applied them the other way round would silently discard what the player
/// rebound. Stated once here so the game and the editor cannot drift.

#include <Assisi/Window/ActionMap.hpp>
#include <Assisi/Window/InputBindings.hpp>

namespace Assisi::App
{

/// @brief Fill @p actions with the shipped bindings, then @p player over the top.
///
/// Apply replaces per action rather than wholesale, so an action the player
/// never touched keeps what shipped — which is what lets a patch add a binding
/// without disturbing a rebind.
///
/// A missing or unreadable shipped file leaves @p actions with whatever it
/// already held and the player's layer still applies: a game with no bindings is
/// worse than a game with the defaults its author compiled in.
void LoadActionMap(Window::ActionMap &actions, const Window::InputBindings &player);

} // namespace Assisi::App
