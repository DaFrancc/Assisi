/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file UiInputBridge.hpp
/// @brief The window's input in the game UI's terms, and the UI's use of it
/// taken back out of the game's.
///
/// Mondrian reads no device and links no window, so the host translates each
/// frame: GatherUiInput before the UI runs, ApplyUiResult after. The input
/// mode decides how much the UI is given; the result says what it used.

#include <Assisi/Mondrian/Input.hpp>
#include <Assisi/Window/ActionMap.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/WindowContext.hpp>

#include <array>
#include <string_view>

#include <glm/glm.hpp>

namespace Assisi::App
{

/// The action names the UI's actions are bound under, by Mondrian::UiAction.
inline constexpr std::array<std::string_view, Mondrian::kUiActionCount> kUiActionNames{
    "UiUp", "UiDown", "UiLeft", "UiRight", "UiAccept", "UiBack", "UiNext", "UiPrevious"};

/// @brief What something drawn over the game UI has taken this frame — the
/// editor's panels — which neither the UI nor the game then sees.
struct InputClaim
{
    bool pointer = false;
    bool keyboard = false;
};

/// @brief How much of the input the UI is given in @p mode.
[[nodiscard]] Mondrian::InputGrant GrantFor(Window::InputMode mode);

/// @brief @p position in window coordinates as the device pixels layout places
/// nodes in. The two differ wherever the framebuffer is not the window's size,
/// as on a high-density display. A window with no size leaves it unscaled.
[[nodiscard]] Mondrian::Point ToDevicePixels(glm::vec2 position, Window::WindowSize window,
                                             Window::WindowSize framebuffer);

/// @brief This frame's input for the UI, at @p time in seconds, with the pointer
/// where @p pointer says, claimed where @p claim says.
///
/// Counts input already consumed: a direction the UI took last frame is still
/// held, and the UI has to see it held to repeat it.
[[nodiscard]] Mondrian::UiInput GatherUiInput(const Window::InputContext &input, const Window::ActionMap &actions,
                                              double time, Mondrian::Point pointer, InputClaim claim);

/// @brief Hides from the game what the UI used and what was claimed over it.
///
/// Where the UI has everything, the game sees nothing. Where it has the
/// pointer, the game loses only a press the UI took and, while the UI holds the
/// keyboard, the keys.
void ApplyUiResult(Window::InputContext &input, const Mondrian::InputResult &result, InputClaim claim);

} // namespace Assisi::App
