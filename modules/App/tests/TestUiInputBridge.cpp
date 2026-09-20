/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/UiInputBridge.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

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

TEST_CASE("UiInputBridge: the wheel reaches the UI, and leaves the game only when a control took it")
{
    const Window::ActionMap actions = UiActions();
    Window::InputContext input;
    input.SetInputMode(InputMode::GameAndUi);
    input.OnScroll(0.0, 1.0);
    input.OnMouseButton(MouseButton::Left, KeyAction::Press, 0.0);
    input.Poll();

    CHECK(App::GatherUiInput(input, actions, 0.0, {}, {}).wheel.y == doctest::Approx(1.f));

    App::ApplyUiResult(input, {}, {});
    CHECK(input.ScrollDelta().y == doctest::Approx(1.f));

    App::ApplyUiResult(input, {.pointerUsed = false, .keyboardTaken = false, .wheelUsed = true}, {});
    CHECK(input.ScrollDelta().y == doctest::Approx(0.f));
    CHECK(input.IsMouseButtonPressed(MouseButton::Left)); // a wheel the UI used is not a click
}

TEST_CASE("UiInputBridge: Shift and the wheel together scroll sideways")
{
    const Window::ActionMap actions = UiActions();
    Window::InputContext input;
    input.SetInputMode(InputMode::GameAndUi);
    input.OnKey(Key::LeftShift, KeyAction::Press, 0.0);
    input.OnScroll(0.0, 1.0);
    input.Poll();

    const Mondrian::UiInput gathered = App::GatherUiInput(input, actions, 0.0, {}, {});
    CHECK(gathered.wheel.x == doctest::Approx(1.f));
    CHECK(gathered.wheel.y == doctest::Approx(0.f));
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

TEST_CASE("UiInputBridge: what was typed reaches the UI as UTF-8")
{
    Window::InputContext input;
    input.SetInputMode(InputMode::Ui);
    input.OnCharacter(U'h');
    input.OnCharacter(U'é');
    input.Poll();

    const Mondrian::UiInput gathered = App::GatherUiInput(input, UiActions(), 0.0, {}, {});
    CHECK(gathered.typed == "h\xC3\xA9");
}

TEST_CASE("UiInputBridge: the editing keys reach the UI as the edits they stand for")
{
    struct Case
    {
        Key key;
        Mondrian::EditKey edit;
        bool control = false;
    };
    const Case cases[] = {
        {.key = Key::Home, .edit = Mondrian::EditKey::LineStart},
        {.key = Key::End, .edit = Mondrian::EditKey::LineEnd},
        {.key = Key::Backspace, .edit = Mondrian::EditKey::Backspace},
        {.key = Key::Delete, .edit = Mondrian::EditKey::Delete},
        {.key = Key::A, .edit = Mondrian::EditKey::SelectAll, .control = true},
        {.key = Key::C, .edit = Mondrian::EditKey::Copy, .control = true},
        {.key = Key::X, .edit = Mondrian::EditKey::Cut, .control = true},
        {.key = Key::V, .edit = Mondrian::EditKey::Paste, .control = true},
    };

    for (const Case &c : cases)
    {
        CAPTURE(static_cast<int32_t>(c.key));
        Window::InputContext input;
        input.SetInputMode(InputMode::Ui);
        if (c.control)
        {
            input.OnKey(Key::LeftControl, KeyAction::Press, 0.0);
        }
        input.OnKey(c.key, KeyAction::Press, 0.0);
        input.Poll();

        const Mondrian::UiInput gathered = App::GatherUiInput(input, UiActions(), 0.0, {}, {});
        for (std::size_t edit = 0; edit < Mondrian::kEditKeyCount; ++edit)
        {
            const bool wanted = edit == static_cast<std::size_t>(c.edit);
            CHECK(gathered.editPressed[edit] == wanted);
        }
    }
}

TEST_CASE("UiInputBridge: Control turns an editing key into the same edit reaching a word")
{
    // Control says how far an edit goes; it must not stop the key meaning what
    // it means. Ctrl+Delete is still Delete.
    for (const Key key : {Key::Delete, Key::Backspace, Key::Home, Key::End})
    {
        CAPTURE(static_cast<int32_t>(key));
        Window::InputContext input;
        input.SetInputMode(InputMode::Ui);
        input.OnKey(Key::LeftControl, KeyAction::Press, 0.0);
        input.OnKey(key, KeyAction::Press, 0.0);
        input.Poll();

        const Mondrian::UiInput gathered = App::GatherUiInput(input, UiActions(), 0.0, {}, {});
        CHECK(gathered.step == Mondrian::TextStep::Word);
        const bool anyEdit = std::ranges::any_of(gathered.editPressed, [](bool pressed) { return pressed; });
        CHECK(anyEdit);
    }
}

TEST_CASE("UiInputBridge: a key the system repeats erases again, at the player's own rate")
{
    Window::InputContext input;
    input.SetInputMode(InputMode::Ui);
    input.OnKey(Key::Backspace, KeyAction::Press, 0.0);
    input.Poll();
    REQUIRE(App::GatherUiInput(input, UiActions(), 0.0, {}, {})
                .editPressed[static_cast<std::size_t>(Mondrian::EditKey::Backspace)]);

    input.OnKey(Key::Backspace, KeyAction::Repeat, 0.1);
    input.Poll();
    CHECK(App::GatherUiInput(input, UiActions(), 0.1, {}, {})
              .editPressed[static_cast<std::size_t>(Mondrian::EditKey::Backspace)]);

    // Held, but with no repeat from the system this frame: nothing fires.
    input.Poll();
    CHECK_FALSE(App::GatherUiInput(input, UiActions(), 0.2, {}, {})
                    .editPressed[static_cast<std::size_t>(Mondrian::EditKey::Backspace)]);
}

TEST_CASE("UiInputBridge: a letter without Control is text rather than an edit")
{
    Window::InputContext input;
    input.SetInputMode(InputMode::Ui);
    input.OnKey(Key::C, KeyAction::Press, 0.0);
    input.OnCharacter(U'c');
    input.Poll();

    const Mondrian::UiInput gathered = App::GatherUiInput(input, UiActions(), 0.0, {}, {});
    CHECK(gathered.typed == "c");
    CHECK_FALSE(gathered.editPressed[static_cast<std::size_t>(Mondrian::EditKey::Copy)]);
}

TEST_CASE("UiInputBridge: Shift extends a selection and Control moves by words")
{
    Window::InputContext input;
    input.SetInputMode(InputMode::Ui);
    input.Poll();
    CHECK(App::GatherUiInput(input, UiActions(), 0.0, {}, {}).reach == Mondrian::TextReach::Moves);
    CHECK(App::GatherUiInput(input, UiActions(), 0.0, {}, {}).step == Mondrian::TextStep::Character);

    input.OnKey(Key::LeftShift, KeyAction::Press, 0.0);
    input.OnKey(Key::RightControl, KeyAction::Press, 0.0);
    input.Poll();

    const Mondrian::UiInput gathered = App::GatherUiInput(input, UiActions(), 0.0, {}, {});
    CHECK(gathered.reach == Mondrian::TextReach::Extends);
    CHECK(gathered.step == Mondrian::TextStep::Word);
}

TEST_CASE("UiInputBridge: text the UI took is hidden from the game")
{
    Window::InputContext input;
    input.SetInputMode(InputMode::GameAndUi);
    input.OnCharacter(U'x');
    input.Poll();

    App::ApplyUiResult(input, {.keyboardTaken = true}, {});
    CHECK(input.TypedCharacters().empty());
}
