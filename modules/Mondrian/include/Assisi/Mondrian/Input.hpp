/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Input.hpp
/// @brief What the host hands the UI each frame, and what the UI hands back.
///
/// The UI never reads a device. The host gathers a UiInput from its own input,
/// in the UI's terms — actions rather than keys, device pixels rather than
/// window coordinates — and hides from the game whatever the InputResult says
/// the UI used.

#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Mondrian/NodeTree.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace Assisi::Mondrian
{

/// @brief What the keyboard, or later a gamepad, asks of the UI.
enum class UiAction : uint8_t
{
    Up,
    Down,
    Left,
    Right,
    Accept,
    Back,
    Next,     ///< Tab order forward
    Previous, ///< Tab order backward
    Count
};

inline constexpr std::size_t kUiActionCount = static_cast<std::size_t>(UiAction::Count);

/// @brief How much of the input the host gives the UI this frame.
enum class InputGrant : uint8_t
{
    Nothing,    ///< the game has it all; the UI only shows
    Pointer,    ///< the pointer, and the keyboard only while a node that takes it has focus
    Everything, ///< the game gets nothing
    Count
};

/// @brief Which device the player used last. The focus ring shows only for
/// keys, since someone pointing can see where they are.
enum class InputDevice : uint8_t
{
    Pointer,
    Keys,
    Count
};

/// @brief One frame of input, as the host gathered it.
struct UiInput
{
    double time = 0.0; ///< seconds, on any clock that only moves forward
    Point pointer;     ///< device pixels, the space layout places nodes in
    std::array<bool, kUiActionCount> actionPressed{};
    std::array<bool, kUiActionCount> actionDown{};
    InputGrant grant = InputGrant::Nothing;
    bool primaryDown = false;
    bool primaryPressed = false;
    bool primaryReleased = false;
    bool pointerClaimed = false;  ///< something over the UI has the pointer this frame
    bool keyboardClaimed = false; ///< something over the UI has the keyboard this frame
};

/// @brief What the UI used, for the host to hide from the game.
struct InputResult
{
    bool pointerUsed = false;   ///< the primary button's press or release was the UI's
    bool keyboardTaken = false; ///< the UI read the keyboard this frame
};

/// @brief Where the pointer and focus are, as of the last ProcessInput.
struct Interaction
{
    NodeId hovered; ///< the focusable, enabled node under the pointer
    NodeId pressed; ///< the node a held press began on, which keeps it until release
    NodeId focused;
    NodeId activated; ///< the node clicked or accepted this frame
    InputDevice device = InputDevice::Pointer;
    bool backPressed = false;
};

/// Seconds a direction is held before it repeats, and between repeats after.
inline constexpr double kNavRepeatDelaySeconds = 0.4;
inline constexpr double kNavRepeatIntervalSeconds = 0.1;

} // namespace Assisi::Mondrian
