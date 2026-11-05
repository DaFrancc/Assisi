/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TuningScreen.hpp
/// @brief F9 shows a small screen of controls bound to fields, written as a
/// file: one writing a component, and others writing a struct on no entity that
/// the HUD reads too.
///
/// Everything it does is in ui/Tuning.amdn. What is here is what a file cannot
/// say: where the struct lives, which camera it tunes, and the key.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

#include <cstdint>
#include <string_view>

namespace Game
{

/// Where the file sits in the package, which is also how the screen is found
/// again.
inline constexpr std::string_view kTuningScreenPath = "ui/Tuning.amdn";

/// A choice that reads as a word rather than a number. Nothing plays differently
/// for it yet: the HUD only shows it.
AENUM()
enum class Difficulty : uint8_t
{
    Easy,
    Normal,
    Hard,
    Count_
};

/// What the tuning screen and the HUD share. On no entity, so no level or
/// player owns it: the UI reaches it as a registered source.
ASTRUCT()
struct TuningSettings
{
    AFIELD() Difficulty difficulty = Difficulty::Normal;
    AFIELD() bool showHud = true;
};

/// Pushed by the tuning screen's Difficulty slider, carrying which step it
/// reached.
AEVENT()
struct DifficultyChanged
{
    int32_t value = 0;
};

/// Gives the world the tuning screen, hidden, and the UI the settings it binds.
ASYSTEM(Loaded, name = "TuningScreen")
void TuningScreenSystem(Assisi::App::SystemContext &ctx);

/// F9 points the screen at the active camera and shows it, and hides it again.
///
/// `activeWorldOnly`: the key belongs to the world on screen, not to every
/// resident one.
ASYSTEM(Update, name = "TuningScreenToggle", activeWorldOnly)
void TuningScreenToggleSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
