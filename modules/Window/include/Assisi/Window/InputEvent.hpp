/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file InputEvent.hpp
/// @brief The input events a window delivers to its subscribers, free of GLFW.

#include <Assisi/Window/Key.hpp>

#include <cstdint>

namespace Assisi::Window
{

/// The longest gap between two presses of one key or button that still
/// continues a run of taps, in seconds, until a game or player sets another.
inline constexpr double kDefaultMultiTapSeconds = 0.3;

/// The interval's bounds. Below the lower no person taps twice; above the upper
/// two deliberate single presses would merge.
inline constexpr double kMinMultiTapSeconds = 0.05;
inline constexpr double kMaxMultiTapSeconds = 2.0;

/// @brief What happened to a key or button. The values are GLFW's.
enum class KeyAction : int32_t
{
    Release,
    Press,
    Repeat, ///< the OS repeating a held key; never sent for a mouse button
    Count
};

/// @brief The modifier keys held, and the lock states on, when an event happened.
struct Modifiers
{
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool super = false;
    bool capsLock = false;
    bool numLock = false;
};

struct KeyEvent
{
    double time = 0.0; ///< when it happened, in seconds on the window library's clock
    Key key = Key::Space;
    int32_t scancode = 0; ///< the platform's code for the physical key
    KeyAction action = KeyAction::Press;
    Modifiers modifiers;
};

struct MouseButtonEvent
{
    double time = 0.0; ///< when it happened, in seconds on the window library's clock
    MouseButton button = MouseButton::Left;
    KeyAction action = KeyAction::Press;
    Modifiers modifiers;
};

} // namespace Assisi::Window
