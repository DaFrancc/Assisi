/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/Ui.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <memory>
#include <string_view>
#include <vector>

using namespace Assisi::Mondrian;

namespace
{

constexpr Extent kViewport{1280, 720};

using Color = Assisi::Math::Color4<Assisi::Math::ColorSpace::Srgb>;

/// Colours that nothing else draws, so a quad can be traced to the screen it
/// came from.
constexpr Color kFirstColor{1.f, 0.f, 0.f, 1.f};
constexpr Color kSecondColor{0.f, 1.f, 0.f, 1.f};

/// A button big enough to aim at, floating so it sits at a known place.
constexpr float kButtonSide = 200.f;

/// The shapes the tests use, spelled once. A menu covers what it opened over
/// and stops the world; a dialog interrupts without hiding it; a HUD is looked
/// at and nothing else, which is the default; a locked screen has the keys and
/// will not give them back to Back.
constexpr ScreenTraits kMenu{
    .input = ScreenInput::ConsumeInput, .beneath = ScreenBeneath::HidesBeneath, .pause = ScreenPause::Pause};
constexpr ScreenTraits kDialog{
    .input = ScreenInput::ConsumeInput, .beneath = ScreenBeneath::NoHide, .pause = ScreenPause::Pause};
constexpr ScreenTraits kHud{};
constexpr ScreenTraits kLocked{
    .input = ScreenInput::LockedConsumeInput, .beneath = ScreenBeneath::NoHide, .pause = ScreenPause::Pause};

/// A UI and the screens a test made, destroyed before it so no screen outlives
/// the UI it registered with. Declaration order is the guarantee: members go in
/// reverse, so `owned` empties first.
struct Stage
{
    Assisi::Core::EventQueue events;
    Ui ui{events};
    std::vector<std::unique_ptr<Screen>> owned;

    /// A screen behaving as @p traits say, at @p sortKey, its root filled with
    /// @p color so its quads can be told from another's.
    Screen &Add(ScreenTraits traits, int32_t sortKey, std::string_view name, const Color &color)
    {
        owned.push_back(std::make_unique<Screen>(ui, traits, sortKey, std::string{name}));
        Screen &screen = *owned.back();

        Style root;
        root.background = color;
        screen.Tree().SetStyle(screen.Root(), root);
        return screen;
    }

    /// Destroys @p screen, as a world does when it goes.
    void Drop(Screen &screen)
    {
        std::erase_if(owned, [&screen](const std::unique_ptr<Screen> &held) { return held.get() == &screen; });
    }

    /// Runs one frame and returns what it drew.
    const DrawList &Frame()
    {
        ui.ProcessInput({});
        ui.Sync(kViewport);
        return ui.GetDrawList();
    }
};

/// One button on @p screen called @p name, floated to a fixed place so every
/// screen's button lands on the same pixels and a press could have gone to
/// either.
NodeId Aim(Screen &screen, std::string_view name)
{
    Style style;
    style.floating.enabled = true;
    style.sizing = {Sizing::Fixed(kButtonSide), Sizing::Fixed(kButtonSide)};
    const std::expected<ButtonId, NameError> id = screen.AddButton(screen.Root(), name, name);
    REQUIRE(id.has_value());
    screen.Tree().SetStyle(id->node, style);
    return id->node;
}

/// The centre of @p name on @p screen, as the last frame placed it.
Point CentreOf(const Screen &screen, std::string_view name)
{
    const LayoutNode *node = screen.GetLayout().Get(screen.Find(name));
    REQUIRE(node != nullptr);
    return {.x = node->rect.x + (node->rect.width / 2.f), .y = node->rect.y + (node->rect.height / 2.f)};
}

UiInput PointerAt(Point pointer)
{
    UiInput input;
    input.pointer = pointer;
    input.grant = InputGrant::Everything;
    return input;
}

UiInput Pressing(Point pointer)
{
    UiInput input = PointerAt(pointer);
    input.primaryDown = true;
    input.primaryPressed = true;
    return input;
}

UiInput Releasing(Point pointer)
{
    UiInput input = PointerAt(pointer);
    input.primaryReleased = true;
    return input;
}

UiInput Action(UiAction action)
{
    UiInput input;
    input.grant = InputGrant::Everything;
    input.actionPressed[static_cast<std::size_t>(action)] = true;
    input.actionDown = input.actionPressed;
    return input;
}

/// Whether anything in @p drawn is filled with @p color.
bool DrawsColor(const DrawList &drawn, const Color &color)
{
    return std::ranges::any_of(drawn.Instances(), [&color](const QuadInstance &quad) { return quad.color == color; });
}

/// Where the first quad filled with @p color sits in the draw list, or the size
/// of the list when there is none.
std::size_t FirstIndexOf(const DrawList &drawn, const Color &color)
{
    const std::span<const QuadInstance> quads = drawn.Instances();
    const std::span<const QuadInstance>::iterator at =
        std::ranges::find_if(quads, [&color](const QuadInstance &quad) { return quad.color == color; });
    return static_cast<std::size_t>(std::ranges::distance(quads.begin(), at));
}

} // namespace

TEST_CASE("Screens: a new screen is hidden, and showing it gives it the keys")
{
    Stage stage;
    CHECK_FALSE(stage.ui.TakesInput());

    Screen &menu = stage.Add(kMenu, kSortMenu, "menu", kFirstColor);
    Aim(menu, "Resume");
    CHECK_FALSE(menu.IsShown());
    CHECK(stage.ui.InputScreen() == nullptr);

    menu.Show();
    CHECK(menu.IsShown());
    CHECK(stage.ui.InputScreen() == &menu);
    CHECK(stage.ui.TakesInput());

    menu.Hide();
    CHECK_FALSE(menu.IsShown());
    CHECK(stage.ui.InputScreen() == nullptr);
    CHECK_FALSE(stage.ui.TakesInput());
}

TEST_CASE("Screens: only the topmost screen that consumes input is hit")
{
    Stage stage;
    Screen &under = stage.Add(kMenu, kSortMenu, "under", kFirstColor);
    Aim(under, "Under");
    // A dialog, so what it covers stays drawn and laid out: both buttons are
    // floated to the same place and the press could land on either.
    Screen &over = stage.Add(kDialog, kSortPopup, "over", kSecondColor);
    Aim(over, "Over");
    under.Show();
    over.Show();
    stage.Frame();
    REQUIRE(stage.ui.InputScreen() == &over);
    REQUIRE(under.GetLayout().Get(under.Find("Under")) != nullptr);

    const Point where = CentreOf(over, "Over");
    REQUIRE(where.x == CentreOf(under, "Under").x);
    REQUIRE(where.y == CentreOf(under, "Under").y);

    stage.ui.ProcessInput(Pressing(where));
    stage.ui.Sync(kViewport);
    CHECK(stage.ui.GetInteraction().pressed == over.Find("Over"));

    stage.ui.ProcessInput(Releasing(where));
    stage.ui.Sync(kViewport);
    CHECK(stage.ui.GetInteraction().activated == over.Find("Over"));
}

TEST_CASE("Screens: a screen remembers its focus and has it back when it returns")
{
    Stage stage;
    Screen &pause = stage.Add(kMenu, kSortMenu, "pause", kFirstColor);
    Aim(pause, "Resume");
    Screen &settings = stage.Add(kMenu, kSortMenu, "settings", kSecondColor);
    Aim(settings, "Back");
    pause.Show();
    stage.Frame();

    const NodeId resume = pause.Find("Resume");
    stage.ui.SetFocus(pause, resume);
    stage.Frame();
    REQUIRE(stage.ui.GetInteraction().focused == resume);

    // Opening a screen over it hands the keys on, and the ids of the screen
    // left behind go with them.
    settings.Show();
    stage.Frame();
    CHECK(stage.ui.InputScreen() == &settings);
    CHECK(stage.ui.GetInteraction().focused != resume);

    // Popping comes back to the pause menu, not past it, with focus where the
    // player left it.
    CHECK(stage.ui.Back());
    stage.Frame();
    CHECK_FALSE(settings.IsShown());
    CHECK(pause.IsShown());
    CHECK(stage.ui.InputScreen() == &pause);
    CHECK(stage.ui.GetInteraction().focused == resume);
}

TEST_CASE("Screens: Back closes only a screen whose traits say so")
{
    Stage stage;
    Screen &hud = stage.Add(kHud, kSortHud, "hud", kFirstColor);
    Aim(hud, "Health");
    hud.Show();
    stage.Frame();

    // A HUD consumes nothing, so there is nothing for Back to close.
    CHECK_FALSE(stage.ui.Back());
    CHECK(hud.IsShown());

    Screen &menu = stage.Add(kMenu, kSortMenu, "menu", kSecondColor);
    Aim(menu, "Resume");
    menu.Show();
    stage.Frame();
    CHECK(stage.ui.Back());
    CHECK_FALSE(menu.IsShown());
    CHECK(hud.IsShown()); // Back took the menu and stopped there
}

TEST_CASE("Screens: a locked screen has the keys and Back does not take them back")
{
    // A screen nobody may dismiss: a prompt that must be answered, a step of a
    // wizard. It consumes input like a menu and ignores Back, so only code
    // hides it.
    Stage stage;
    Screen &locked = stage.Add(kLocked, kSortMenu, "locked", kFirstColor);
    Aim(locked, "OK");
    locked.Show();
    stage.Frame();

    REQUIRE(stage.ui.InputScreen() == &locked);
    CHECK(stage.ui.TakesInput());

    CHECK_FALSE(stage.ui.Back());
    CHECK(locked.IsShown());

    // The Back key reaches it and changes nothing either.
    stage.ui.ProcessInput(Action(UiAction::Back));
    stage.ui.Sync(kViewport);
    CHECK(locked.IsShown());

    locked.Hide();
    CHECK_FALSE(locked.IsShown());
}

TEST_CASE("Screens: the sort key decides draw order, and show order decides within a key")
{
    SUBCASE("a higher sort key draws later however the screens were shown")
    {
        Stage stage;
        Screen &popup = stage.Add(kDialog, kSortPopup, "popup", kSecondColor);
        Screen &hud = stage.Add(kHud, kSortHud, "hud", kFirstColor);
        popup.Show(); // shown first, but sorts above
        hud.Show();
        const DrawList &drawn = stage.Frame();

        REQUIRE(DrawsColor(drawn, kFirstColor));
        REQUIRE(DrawsColor(drawn, kSecondColor));
        CHECK(FirstIndexOf(drawn, kFirstColor) < FirstIndexOf(drawn, kSecondColor));
    }

    SUBCASE("sharing a key, the one shown later draws later")
    {
        Stage stage;
        Screen &first = stage.Add(kDialog, kSortPopup, "first", kFirstColor);
        Screen &second = stage.Add(kDialog, kSortPopup, "second", kSecondColor);
        first.Show();
        second.Show();
        const DrawList &drawn = stage.Frame();

        REQUIRE(DrawsColor(drawn, kSecondColor));
        CHECK(FirstIndexOf(drawn, kFirstColor) < FirstIndexOf(drawn, kSecondColor));
    }
}

TEST_CASE("Screens: a screen that hides what is beneath it stops the rest being drawn")
{
    Stage stage;
    Screen &hud = stage.Add(kHud, kSortHud, "hud", kFirstColor);
    hud.Show();
    REQUIRE(DrawsColor(stage.Frame(), kFirstColor));

    // A menu covers the screen, so what sorts below it is not drawn.
    Screen &menu = stage.Add(kMenu, kSortMenu, "menu", kSecondColor);
    menu.Show();
    const DrawList &covered = stage.Frame();
    CHECK(DrawsColor(covered, kSecondColor));
    CHECK_FALSE(DrawsColor(covered, kFirstColor));

    // A dialog does not, so both it and what it interrupts stay in sight.
    menu.Hide();
    Screen &popup = stage.Add(kDialog, kSortPopup, "popup", kSecondColor);
    popup.Show();
    const DrawList &both = stage.Frame();
    CHECK(DrawsColor(both, kFirstColor));
    CHECK(DrawsColor(both, kSecondColor));
}

TEST_CASE("Screens: what is hidden beneath is not laid out either")
{
    // Not drawn is the visible half; not laid out is the point. A screen nobody
    // can see must not cost a layout pass every frame.
    Stage stage;
    Screen &hud = stage.Add(kHud, kSortHud, "hud", kFirstColor);
    Aim(hud, "Health");
    hud.Show();
    stage.Frame();
    REQUIRE(hud.GetLayout().Get(hud.Find("Health")) != nullptr);
    const float laidOut = hud.GetLayout().scale;
    REQUIRE(laidOut > 0.f);

    Screen &menu = stage.Add(kMenu, kSortMenu, "menu", kSecondColor);
    menu.Show();

    // A viewport of a different size would change the scale of anything that
    // was laid out; the covered screen keeps the scale it had.
    stage.ui.ProcessInput({});
    stage.ui.Sync({kViewport.width / 2, kViewport.height / 2});
    CHECK(hud.GetLayout().scale == laidOut);
    CHECK(menu.GetLayout().scale != laidOut);
}

TEST_CASE("Screens: what is shown decides whether the UI takes input and stops the world")
{
    Stage stage;
    Screen &hud = stage.Add(kHud, kSortHud, "hud", kFirstColor);
    Screen &menu = stage.Add(kMenu, kSortMenu, "menu", kSecondColor);

    CHECK_FALSE(stage.ui.TakesInput());
    CHECK_FALSE(stage.ui.PausesWorld());

    // A HUD is looked at, not worked, and a game goes on running behind it.
    hud.Show();
    CHECK_FALSE(stage.ui.TakesInput());
    CHECK_FALSE(stage.ui.PausesWorld());

    menu.Show();
    CHECK(stage.ui.TakesInput());
    CHECK(stage.ui.PausesWorld());

    menu.Hide();
    CHECK_FALSE(stage.ui.TakesInput());
    CHECK_FALSE(stage.ui.PausesWorld());
}

TEST_CASE("Screens: consuming input and stopping the world are separate answers")
{
    // An inventory a player works while the world runs on around them — a
    // combination the four kinds this replaced could not express.
    Stage stage;
    Screen &inventory =
        stage.Add({.input = ScreenInput::ConsumeInput, .beneath = ScreenBeneath::NoHide, .pause = ScreenPause::Run},
                  kSortMenu, "inventory", kFirstColor);
    inventory.Show();

    CHECK(stage.ui.TakesInput());
    CHECK_FALSE(stage.ui.PausesWorld());

    // And the other way: a letterbox that stops the world and is worked by
    // nothing.
    Screen &cutscene = stage.Add(
        {.input = ScreenInput::NoConsume, .beneath = ScreenBeneath::HidesBeneath, .pause = ScreenPause::Pause},
        kSortOverlay, "cutscene", kSecondColor);
    inventory.Hide();
    cutscene.Show();

    CHECK_FALSE(stage.ui.TakesInput());
    CHECK(stage.ui.PausesWorld());
}

TEST_CASE("Screens: showing another screen drops the hover and press the last one had")
{
    Stage stage;
    Screen &first = stage.Add(kMenu, kSortMenu, "first", kFirstColor);
    Aim(first, "One");
    Screen &second = stage.Add(kMenu, kSortMenu, "second", kSecondColor);
    Aim(second, "Two");
    first.Show();
    stage.Frame();

    stage.ui.ProcessInput(Pressing(CentreOf(first, "One")));
    stage.ui.Sync(kViewport);
    REQUIRE(stage.ui.GetInteraction().pressed == first.Find("One"));
    REQUIRE(stage.ui.GetInteraction().hovered == first.Find("One"));

    // The ids named slots in the first screen's tree; the same slots in the
    // second hold different nodes, so none of them may survive the handover.
    second.Show();
    CHECK_FALSE(stage.ui.GetInteraction().pressed);
    CHECK_FALSE(stage.ui.GetInteraction().hovered);
    CHECK_FALSE(stage.ui.GetInteraction().activated);
}

TEST_CASE("Screens: a button may close the screen it is on")
{
    // The half of a screen's behaviour that reaches nothing outside the UI, so
    // it works with no event, no system, and nothing named in a level.
    //
    // It runs while the UI is announcing the frame's callbacks, and hiding a
    // screen clears the very lists that announcing walks — which is why those
    // are taken by copy first.
    Stage stage;
    Screen &menu = stage.Add(kMenu, kSortMenu, "menu", kFirstColor);
    const NodeId resume = Aim(menu, "Resume");
    menu.OnActivate(resume, [](Screen &self) { self.Hide(); });
    menu.Show();
    stage.Frame();

    const Point where = CentreOf(menu, "Resume");
    stage.ui.ProcessInput(Pressing(where));
    stage.ui.Sync(kViewport);
    REQUIRE(menu.IsShown());

    stage.ui.ProcessInput(Releasing(where));
    stage.ui.Sync(kViewport);
    CHECK_FALSE(menu.IsShown());
    CHECK(stage.ui.InputScreen() == nullptr);

    // The frame after still runs.
    CHECK_NOTHROW(stage.Frame());
}

TEST_CASE("Screens: destroying a screen hands the keys to whatever is left")
{
    Stage stage;
    Screen &pause = stage.Add(kMenu, kSortMenu, "pause", kFirstColor);
    Aim(pause, "Resume");
    Screen &settings = stage.Add(kMenu, kSortMenu, "settings", kSecondColor);
    Aim(settings, "Back");
    pause.Show();
    settings.Show();
    stage.Frame();
    REQUIRE(stage.ui.InputScreen() == &settings);

    stage.Drop(settings);
    CHECK(stage.ui.InputScreen() == &pause);
    CHECK(stage.ui.TakesInput());

    // The frame after still runs: nothing is left pointing into what is gone.
    CHECK_NOTHROW(stage.Frame());
}

TEST_CASE("Screens: a destroyed screen leaves the UI with nothing to draw")
{
    Stage stage;
    Screen &menu = stage.Add(kMenu, kSortMenu, "menu", kFirstColor);
    menu.Show();
    REQUIRE_FALSE(stage.Frame().Instances().empty());

    stage.Drop(menu);
    CHECK(stage.Frame().Instances().empty());
    CHECK(stage.ui.InputScreen() == nullptr);
    CHECK_FALSE(stage.ui.TakesInput());
    CHECK_FALSE(stage.ui.PausesWorld());
}

TEST_CASE("Screens: with nothing shown the UI draws nothing and takes no input")
{
    Stage stage;
    Screen &menu = stage.Add(kMenu, kSortMenu, "menu", kFirstColor);
    Aim(menu, "Resume");
    CHECK(stage.Frame().Instances().empty());

    const InputResult result = stage.ui.ProcessInput(Pressing({.x = 100.f, .y = 100.f}));
    stage.ui.Sync(kViewport);
    CHECK_FALSE(result.pointerUsed);
    CHECK_FALSE(result.keyboardTaken);

    menu.Show();
    CHECK_FALSE(stage.Frame().Instances().empty());
}

TEST_CASE("Screens: Accept reaches the screen with the keys, and Back closes it")
{
    Stage stage;
    Screen &menu = stage.Add(kMenu, kSortMenu, "menu", kFirstColor);
    const NodeId resume = Aim(menu, "Resume");
    menu.Show();
    stage.Frame();

    stage.ui.SetFocus(menu, resume);
    stage.ui.ProcessInput(Action(UiAction::Accept));
    stage.ui.Sync(kViewport);
    CHECK(stage.ui.GetInteraction().activated == resume);

    stage.ui.ProcessInput(Action(UiAction::Back));
    stage.ui.Sync(kViewport);
    CHECK_FALSE(menu.IsShown());
}
