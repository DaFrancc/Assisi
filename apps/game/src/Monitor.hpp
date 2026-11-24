/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Monitor.hpp
/// @brief Using a screen shown on a surface in the world: the book's example of
///        App::UseScreenSurface.
///
/// The player walks up to the monitor in levels/Test.alvl and presses E while
/// looking at it. Its screen takes the keys and the pointer until Escape.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

namespace Game
{

/// E uses the screen surface the camera looks at, within reach. `activeWorldOnly`,
/// and it null-checks input: a headless host has no keyboard.
ASYSTEM(Update, name = "UseMonitor", activeWorldOnly)
void UseMonitorSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
