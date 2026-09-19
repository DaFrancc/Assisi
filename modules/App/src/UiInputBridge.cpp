/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/UiInputBridge.hpp>

#include <cstddef>

namespace Assisi::App
{

Mondrian::InputGrant GrantFor(Window::InputMode mode)
{
    switch (mode)
    {
    case Window::InputMode::Ui:
        return Mondrian::InputGrant::Everything;
    case Window::InputMode::GameAndUi:
        return Mondrian::InputGrant::Pointer;
    case Window::InputMode::Game:
    case Window::InputMode::Count:
        break;
    }
    return Mondrian::InputGrant::Nothing;
}

Mondrian::Point ToDevicePixels(glm::vec2 position, Window::WindowSize window, Window::WindowSize framebuffer)
{
    if (window.Width <= 0 || window.Height <= 0)
    {
        return {.x = position.x, .y = position.y};
    }
    const float scaleX = static_cast<float>(framebuffer.Width) / static_cast<float>(window.Width);
    const float scaleY = static_cast<float>(framebuffer.Height) / static_cast<float>(window.Height);
    return {.x = position.x * scaleX, .y = position.y * scaleY};
}

Mondrian::UiInput GatherUiInput(const Window::InputContext &input, const Window::ActionMap &actions, double time,
                                Mondrian::Point pointer, InputClaim claim)
{
    constexpr Window::ConsumedInput kAll = Window::ConsumedInput::Include;
    constexpr Window::MouseButton kPrimary = Window::MouseButton::Left;

    Mondrian::UiInput gathered;
    gathered.time = time;
    gathered.pointer = pointer;
    gathered.grant = GrantFor(input.GetInputMode());
    for (std::size_t action = 0; action < Mondrian::kUiActionCount; ++action)
    {
        gathered.actionPressed[action] = actions.IsActionPressed(kUiActionNames[action], input, kAll);
        gathered.actionDown[action] = actions.IsActionDown(kUiActionNames[action], input, kAll);
    }
    // Shift and the wheel is how a mouse without a sideways wheel scrolls
    // sideways, which is what players expect from every other application.
    const glm::vec2 scrolled = input.ScrollDelta();
    const bool sideways =
        input.IsKeyDown(Window::Key::LeftShift, kAll) || input.IsKeyDown(Window::Key::RightShift, kAll);
    gathered.wheel = sideways ? Mondrian::Point{.x = scrolled.x + scrolled.y, .y = 0.f}
                              : Mondrian::Point{.x = scrolled.x, .y = scrolled.y};
    gathered.primaryDown = input.IsMouseButtonDown(kPrimary, kAll);
    gathered.primaryPressed = input.IsMouseButtonPressed(kPrimary, kAll);
    gathered.primaryReleased = input.IsMouseButtonReleased(kPrimary, kAll);
    gathered.pointerClaimed = claim.pointer;
    gathered.keyboardClaimed = claim.keyboard;
    return gathered;
}

void ApplyUiResult(Window::InputContext &input, const Mondrian::InputResult &result, InputClaim claim)
{
    if (claim.pointer)
    {
        input.ConsumeMouse();
    }
    if (claim.keyboard)
    {
        input.ConsumeKeyboard();
    }

    switch (GrantFor(input.GetInputMode()))
    {
    case Mondrian::InputGrant::Everything:
        input.ConsumeAll();
        return;
    case Mondrian::InputGrant::Pointer:
        if (result.pointerUsed)
        {
            input.ConsumeMouseButton(Window::MouseButton::Left);
        }
        if (result.keyboardTaken)
        {
            input.ConsumeKeyboard();
        }
        if (result.wheelUsed)
        {
            input.ConsumeScroll();
        }
        return;
    case Mondrian::InputGrant::Nothing:
    case Mondrian::InputGrant::Count:
        return;
    }
}

} // namespace Assisi::App
