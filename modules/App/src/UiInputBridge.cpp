/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/UiInputBridge.hpp>

#include <Assisi/Mondrian/Utf8.hpp>

#include <array>
#include <cstddef>

namespace Assisi::App
{
namespace
{

/// One editing key and the keys that produce it. These are the platform's
/// conventions rather than a game's choices — nobody rebinds Backspace — so
/// they are spelled here instead of going through the action map.
struct EditChord
{
    Window::Key key = Window::Key::Backspace;
    Mondrian::EditKey edit = Mondrian::EditKey::Backspace;
    /// Whether Control must be held for this key to mean this edit at all.
    /// False does not mean Control must be absent: Control turns Delete into
    /// "delete the word", which is the same edit reaching further.
    bool needsControl = false;
};

constexpr std::array kEditChords{
    EditChord{.key = Window::Key::Home, .edit = Mondrian::EditKey::LineStart},
    EditChord{.key = Window::Key::End, .edit = Mondrian::EditKey::LineEnd},
    EditChord{.key = Window::Key::Backspace, .edit = Mondrian::EditKey::Backspace},
    EditChord{.key = Window::Key::Delete, .edit = Mondrian::EditKey::Delete},
    EditChord{.key = Window::Key::A, .edit = Mondrian::EditKey::SelectAll, .needsControl = true},
    EditChord{.key = Window::Key::C, .edit = Mondrian::EditKey::Copy, .needsControl = true},
    EditChord{.key = Window::Key::X, .edit = Mondrian::EditKey::Cut, .needsControl = true},
    EditChord{.key = Window::Key::V, .edit = Mondrian::EditKey::Paste, .needsControl = true},
};

bool Holding(const Window::InputContext &input, Window::Key left, Window::Key right)
{
    constexpr Window::ConsumedInput kAll = Window::ConsumedInput::Include;
    return input.IsKeyDown(left, kAll) || input.IsKeyDown(right, kAll);
}

} // namespace

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
    for (const char32_t codepoint : input.TypedCharacters())
    {
        Mondrian::EncodeUtf8(static_cast<uint32_t>(codepoint), gathered.typed);
    }

    const bool shift = Holding(input, Window::Key::LeftShift, Window::Key::RightShift);
    const bool control = Holding(input, Window::Key::LeftControl, Window::Key::RightControl);
    gathered.reach = shift ? Mondrian::TextReach::Extends : Mondrian::TextReach::Moves;
    gathered.step = control ? Mondrian::TextStep::Word : Mondrian::TextStep::Character;
    for (const EditChord &chord : kEditChords)
    {
        if (chord.needsControl && !control)
        {
            continue;
        }
        const std::size_t edit = static_cast<std::size_t>(chord.edit);
        // A repeat counts as a press: the window library delivers those at
        // whatever rate the player's own keyboard settings ask for, which is
        // the rate they expect a held backspace to erase at.
        gathered.editPressed[edit] = input.IsKeyPressed(chord.key, kAll) || input.IsKeyRepeated(chord.key, kAll);
        gathered.editDown[edit] = input.IsKeyDown(chord.key, kAll);
    }

    const glm::vec2 scrolled = input.ScrollDelta();
    const bool sideways = shift;
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
