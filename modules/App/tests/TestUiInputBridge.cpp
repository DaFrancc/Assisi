/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/UiInputBridge.hpp>

#include <doctest/doctest.h>

#include <cstddef>

using namespace Assisi;
using Window::InputMode;
using Window::Key;
using Window::KeyAction;
using Window::MouseButton;

namespace
{

constexpr std::size_t At(Mondrian::UiAction action)
{
    return static_cast<std::size_t>(action);
}

/// An action map with the UI's actions on the keys a shipped game binds.
Window::ActionMap UiActions()
{
    Window::ActionMap actions;
    actions.Bind("UiDown", Key::Down);
    actions.Bind("UiAccept", Key::Enter);
    actions.Bind("Jump", Key::Space);
    return actions;
}

} // namespace

TEST_CASE("UiInputBridge: the input mode decides how much the UI is given")
{
    CHECK(App::GrantFor(InputMode::Game) == Mondrian::InputGrant::Nothing);
    CHECK(App::GrantFor(InputMode::GameAndUi) == Mondrian::InputGrant::Pointer);
    CHECK(App::GrantFor(InputMode::Ui) == Mondrian::InputGrant::Everything);
}

TEST_CASE("UiInputBridge: the pointer lands in device pixels when the framebuffer is denser than the window")
{
    const Mondrian::Point point =
        App::ToDevicePixels({100.f, 50.f}, {.Width = 800, .Height = 600}, {.Width = 1600, .Height = 1200});
    CHECK(point.x == doctest::Approx(200.f));
    CHECK(point.y == doctest::Approx(100.f));

    const Mondrian::Point unsized = App::ToDevicePixels({100.f, 50.f}, {}, {.Width = 1600, .Height = 1200});
    CHECK(unsized.x == doctest::Approx(100.f));
}

TEST_CASE("UiInputBridge: the UI sees its actions, a held one even after it consumed it")
{
    const Window::ActionMap actions = UiActions();
    Window::InputContext input;
    input.SetInputMode(InputMode::Ui);
    input.OnKey(Key::Down, KeyAction::Press, 0.0);
    input.OnMouseButton(MouseButton::Left, KeyAction::Press, 0.0);
    input.Poll();

    Mondrian::UiInput gathered = App::GatherUiInput(input, actions, 1.5, {.x = 3.f, .y = 4.f}, {});
    CHECK(gathered.grant == Mondrian::InputGrant::Everything);
    CHECK(gathered.time == 1.5);
    CHECK(gathered.pointer.x == 3.f);
    CHECK(gathered.actionPressed[At(Mondrian::UiAction::Down)]);
    CHECK_FALSE(gathered.actionPressed[At(Mondrian::UiAction::Accept)]);
    CHECK(gathered.primaryPressed);
    CHECK(gathered.primaryDown);

    App::ApplyUiResult(input, {}, {});
    input.Poll();
    gathered = App::GatherUiInput(input, actions, 2.0, {}, {.pointer = true, .keyboard = false});
    CHECK_FALSE(gathered.actionPressed[At(Mondrian::UiAction::Down)]);
    CHECK(gathered.actionDown[At(Mondrian::UiAction::Down)]);
    CHECK(gathered.pointerClaimed);
    CHECK_FALSE(gathered.keyboardClaimed);
}

TEST_CASE("UiInputBridge: while the UI has everything, the game sees nothing")
{
    const Window::ActionMap actions = UiActions();
    Window::InputContext input;
    input.SetInputMode(InputMode::Ui);
    input.OnKey(Key::Space, KeyAction::Press, 0.0);
    input.Poll();

    App::ApplyUiResult(input, {}, {});
    CHECK_FALSE(actions.IsActionPressed("Jump", input));
}

TEST_CASE("UiInputBridge: while the game has the keyboard, it loses only the press the UI took")
{
    const Window::ActionMap actions = UiActions();
    Window::InputContext input;
    input.SetInputMode(InputMode::GameAndUi);
    input.OnKey(Key::Space, KeyAction::Press, 0.0);
    input.OnMouseButton(MouseButton::Left, KeyAction::Press, 0.0);
    input.Poll();

    App::ApplyUiResult(input, {.pointerUsed = true, .keyboardTaken = false}, {});
    CHECK_FALSE(input.IsMouseButtonPressed(MouseButton::Left));
    CHECK(actions.IsActionPressed("Jump", input));

    App::ApplyUiResult(input, {.pointerUsed = false, .keyboardTaken = true}, {});
    CHECK_FALSE(actions.IsActionPressed("Jump", input));
}

TEST_CASE("UiInputBridge: a press the UI did not take is the game's")
{
    Window::InputContext input;
    input.SetInputMode(InputMode::GameAndUi);
    input.OnMouseButton(MouseButton::Left, KeyAction::Press, 0.0);
    input.Poll();

    App::ApplyUiResult(input, {}, {});
    CHECK(input.IsMouseButtonPressed(MouseButton::Left));
}

TEST_CASE("UiInputBridge: what is claimed over the UI is hidden from the game too")
{
    Window::InputContext input;
    input.SetInputMode(InputMode::Game);
    input.OnMouseButton(MouseButton::Left, KeyAction::Press, 0.0);
    input.OnKey(Key::W, KeyAction::Press, 0.0);
    input.Poll();

    App::ApplyUiResult(input, {}, {.pointer = true, .keyboard = false});
    CHECK_FALSE(input.IsMouseButtonPressed(MouseButton::Left));
    CHECK(input.IsKeyPressed(Key::W));

    App::ApplyUiResult(input, {}, {.pointer = false, .keyboard = true});
    CHECK_FALSE(input.IsKeyPressed(Key::W));
}
