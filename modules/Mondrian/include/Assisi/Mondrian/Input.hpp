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
#include <Assisi/Mondrian/Widget.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Assisi::Mondrian
{

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
    /// What the keyboard produced this frame, UTF-8: what a layout, the
    /// modifiers and any dead keys made of the keys pressed, which is what a
    /// text field takes in. Not the same question as which keys are down.
    std::string typed;
    double time = 0.0; ///< seconds, on any clock that only moves forward
    Point pointer;     ///< device pixels, the space layout places nodes in
    std::array<bool, kUiActionCount> actionPressed{};
    std::array<bool, kUiActionCount> actionDown{};
    std::array<bool, kEditKeyCount> editPressed{};
    std::array<bool, kEditKeyCount> editDown{};
    /// Notches scrolled this frame: y away from the player, x sideways, which a
    /// tilting wheel, a trackpad, or the host's Shift and wheel together send.
    Point wheel;
    InputGrant grant = InputGrant::Nothing;
    /// Whether a movement carries the selection with it, and how far it goes:
    /// the Shift and Control the host reads off the keyboard, in the terms the
    /// UI thinks in.
    TextReach reach = TextReach::Moves;
    TextStep step = TextStep::Character;
    bool primaryDown = false;
    bool primaryPressed = false;
    bool primaryReleased = false;
    bool pointerClaimed = false;  ///< something over the UI has the pointer this frame
    bool keyboardClaimed = false; ///< something over the UI has the keyboard this frame
};

/// @brief What the UI used, for the host to hide from the game.
struct InputResult
{
    /// What the pointer should look like, from whatever it is over or has hold
    /// of. Arrow whenever the UI has nothing to say, which includes every
    /// frame the game has the pointer to itself.
    Core::CursorShape cursor = Core::CursorShape::Arrow;
    bool pointerUsed = false;   ///< the primary button's press or release was the UI's
    bool keyboardTaken = false; ///< the UI read the keyboard this frame
    bool wheelUsed = false;     ///< a control scrolled on this frame's wheel
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

/// @brief Pushed when the Back action is pressed while the UI has the keys.
struct UiBack
{
};

/// Seconds a direction is held before it repeats, and between repeats after.
/// The same for an editing key: a backspace held down erases at the rate a
/// direction held down moves.
inline constexpr double kNavRepeatDelaySeconds = 0.4;
inline constexpr double kNavRepeatIntervalSeconds = 0.1;

/// @brief The one key of its kind being held long enough to repeat, and when it
/// next acts. Count for none.
template <typename Key> struct KeyRepeat
{
    double at = 0.0;
    Key key = Key::Count;
};

} // namespace Assisi::Mondrian
