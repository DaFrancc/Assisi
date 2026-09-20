/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SampleScreenDemo.hpp
/// @brief F4 shows one of everything the UI can draw.
///
/// The sample screen holds every kind of sizing, every built-in control and
/// both ways text can be set, so a look at it in two window sizes says whether
/// layout reflows and whether anything draws wrong. It is demo content rather
/// than engine content: it belongs to a world like any other screen, and a
/// level that does not want it does not place the blueprint that asks for it.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Mondrian/SampleScreen.hpp>

namespace Game
{

/// What the screen calls itself, which is how it is found again.
inline constexpr std::string_view kSampleScreenName = Assisi::Mondrian::kSampleScreenName;

/// Gives the world the sample screen, hidden.
///
/// It declares nothing: F4 is read by the system below, which the same
/// blueprint asks for, and everything on the screen is worked by the UI alone.
ASYSTEM(Loaded, name = "SampleScreen")
void SampleScreenSystem(Assisi::App::SystemContext &ctx);

/// F4 shows the sample screen and hides it again.
///
/// `activeWorldOnly`: the key belongs to the world on screen, not to every
/// resident one.
ASYSTEM(Update, name = "SampleScreenToggle", activeWorldOnly)
void SampleScreenToggleSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
