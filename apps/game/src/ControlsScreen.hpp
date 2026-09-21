/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ControlsScreen.hpp
/// @brief F3 shows a screen holding one of every built-in control, written as
/// a file rather than built in code.
///
/// The same controls the sample screen makes through the node API, made instead
/// by the markup loader. A test already compares the two node for node; this is
/// for looking at what they draw, which is the half no assertion covers.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

#include <string_view>

namespace Game
{

/// What the screen calls itself in the file, which is how it is found again.
inline constexpr std::string_view kControlsScreenName = "Controls";

/// Where the file sits in the package.
inline constexpr std::string_view kControlsScreenPath = "ui/Controls.amdn";

/// Gives the world the controls screen, hidden.
///
/// It declares nothing: F3 is read by the system below, which the same
/// blueprint asks for, and everything on the screen is worked by the UI alone.
ASYSTEM(Loaded, name = "ControlsScreen")
void ControlsScreenSystem(Assisi::App::SystemContext &ctx);

/// F3 shows the controls screen and hides it again.
///
/// `activeWorldOnly`: the key belongs to the world on screen, not to every
/// resident one.
ASYSTEM(Update, name = "ControlsScreenToggle", activeWorldOnly)
void ControlsScreenToggleSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
