/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/Ui.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <string_view>

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

/// Runs one frame and returns what it drew.
const DrawList &Frame(Ui &ui)
{
    ui.ProcessInput({});
    ui.Sync(kViewport);
    return ui.GetDrawList();
}

/// A screen of @p kind at @p sortKey whose root is filled with @p color and
/// which carries one button called @p button.
Screen *Build(Ui &ui, ScreenKind kind, int32_t sortKey, std::string_view name, const Color &color,
              std::string_view button)
{
    Screen *screen = ui.CreateScreen(kind, sortKey, name);

    Style root;
    root.background = color;
    screen->Tree().SetStyle(screen->Root(), root);

    Style style;
    style.floating.enabled = true;
    style.sizing = {Sizing::Fixed(kButtonSide), Sizing::Fixed(kButtonSide)};
    const ButtonId id = screen->AddButton(screen->Root(), button);
    screen->Tree().SetStyle(id.node, style);
    return screen;
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
    const auto quads = drawn.Instances();
    const auto at = std::ranges::find_if(quads, [&color](const QuadInstance &quad) { return quad.color == color; });
    return static_cast<std::size_t>(std::ranges::distance(quads.begin(), at));
}

} // namespace

TEST_CASE("Screens: a new screen is hidden, and showing it gives it the keys")
{
    Ui ui;
    CHECK_FALSE(ui.TakesInput());

    Screen *menu = Build(ui, ScreenKind::Stacked, kSortMenu, "menu", kFirstColor, "Resume");
    CHECK_FALSE(menu->IsShown());
    CHECK(ui.InputScreen() == nullptr);

    ui.Show(*menu);
    CHECK(menu->IsShown());
    CHECK(ui.InputScreen() == menu);
    CHECK(ui.TakesInput());

    ui.Hide(*menu);
    CHECK_FALSE(menu->IsShown());
    CHECK(ui.InputScreen() == nullptr);
    CHECK_FALSE(ui.TakesInput());
}

TEST_CASE("Screens: only the topmost screen that takes input is hit")
{
    Ui ui;
    Screen *under = Build(ui, ScreenKind::Stacked, kSortMenu, "under", kFirstColor, "Under");
    // A popup, so what it covers stays drawn and laid out: both buttons are
    // floated to the same place and the press could land on either.
    Screen *over = Build(ui, ScreenKind::Popup, kSortPopup, "over", kSecondColor, "Over");
    ui.Show(*under);
    ui.Show(*over);
    Frame(ui);
    REQUIRE(ui.InputScreen() == over);
    REQUIRE(under->GetLayout().Get(under->Find("Under")) != nullptr);

    const Point where = CentreOf(*over, "Over");
    REQUIRE(where.x == CentreOf(*under, "Under").x);
    REQUIRE(where.y == CentreOf(*under, "Under").y);

    ui.ProcessInput(Pressing(where));
    ui.Sync(kViewport);
    CHECK(ui.GetInteraction().pressed == over->Find("Over"));

    ui.ProcessInput(Releasing(where));
    ui.Sync(kViewport);
    CHECK(ui.GetInteraction().activated == over->Find("Over"));
}

TEST_CASE("Screens: a screen remembers its focus and has it back when it returns")
{
    Ui ui;
    Screen *pause = Build(ui, ScreenKind::Stacked, kSortMenu, "pause", kFirstColor, "Resume");
    Screen *settings = Build(ui, ScreenKind::Stacked, kSortMenu, "settings", kSecondColor, "Back");
    ui.Show(*pause);
    Frame(ui);

    const NodeId resume = pause->Find("Resume");
    ui.SetFocus(*pause, resume);
    Frame(ui);
    REQUIRE(ui.GetInteraction().focused == resume);

    // Opening a screen over it hands the keys on, and the ids of the screen
    // left behind go with them.
    ui.Show(*settings);
    Frame(ui);
    CHECK(ui.InputScreen() == settings);
    CHECK(ui.GetInteraction().focused != resume);

    // Popping comes back to the pause menu, not past it, with focus where the
    // player left it.
    CHECK(ui.Back());
    Frame(ui);
    CHECK_FALSE(settings->IsShown());
    CHECK(pause->IsShown());
    CHECK(ui.InputScreen() == pause);
    CHECK(ui.GetInteraction().focused == resume);
}

TEST_CASE("Screens: Back closes only a screen whose kind says so")
{
    Ui ui;
    Screen *hud = Build(ui, ScreenKind::Persistent, kSortHud, "hud", kFirstColor, "Health");
    ui.Show(*hud);
    Frame(ui);

    // A HUD takes no input, so there is nothing for Back to close.
    CHECK_FALSE(ui.Back());
    CHECK(hud->IsShown());

    Screen *menu = Build(ui, ScreenKind::Stacked, kSortMenu, "menu", kSecondColor, "Resume");
    ui.Show(*menu);
    Frame(ui);
    CHECK(ui.Back());
    CHECK_FALSE(menu->IsShown());
    CHECK(hud->IsShown()); // Back took the menu and stopped there
}

TEST_CASE("Screens: the sort key decides draw order, and show order decides within a key")
{
    SUBCASE("a higher sort key draws later however the screens were shown")
    {
        Ui ui;
        Screen *popup = Build(ui, ScreenKind::Popup, kSortPopup, "popup", kSecondColor, "OK");
        Screen *hud = Build(ui, ScreenKind::Persistent, kSortHud, "hud", kFirstColor, "Health");
        ui.Show(*popup); // shown first, but sorts above
        ui.Show(*hud);
        const DrawList &drawn = Frame(ui);

        REQUIRE(DrawsColor(drawn, kFirstColor));
        REQUIRE(DrawsColor(drawn, kSecondColor));
        CHECK(FirstIndexOf(drawn, kFirstColor) < FirstIndexOf(drawn, kSecondColor));
    }

    SUBCASE("sharing a key, the one shown later draws later")
    {
        Ui ui;
        Screen *first = Build(ui, ScreenKind::Popup, kSortPopup, "first", kFirstColor, "One");
        Screen *second = Build(ui, ScreenKind::Popup, kSortPopup, "second", kSecondColor, "Two");
        ui.Show(*first);
        ui.Show(*second);
        const DrawList &drawn = Frame(ui);

        REQUIRE(DrawsColor(drawn, kSecondColor));
        CHECK(FirstIndexOf(drawn, kFirstColor) < FirstIndexOf(drawn, kSecondColor));
    }
}

TEST_CASE("Screens: a screen that hides what is beneath it stops the rest being drawn")
{
    Ui ui;
    Screen *hud = Build(ui, ScreenKind::Persistent, kSortHud, "hud", kFirstColor, "Health");
    ui.Show(*hud);
    REQUIRE(DrawsColor(Frame(ui), kFirstColor));

    // A stacked menu covers the screen, so what sorts below it is not drawn.
    Screen *menu = Build(ui, ScreenKind::Stacked, kSortMenu, "menu", kSecondColor, "Resume");
    ui.Show(*menu);
    const DrawList &covered = Frame(ui);
    CHECK(DrawsColor(covered, kSecondColor));
    CHECK_FALSE(DrawsColor(covered, kFirstColor));

    // A popup does not, so both it and what it interrupts stay in sight.
    ui.Hide(*menu);
    Screen *popup = Build(ui, ScreenKind::Popup, kSortPopup, "popup", kSecondColor, "OK");
    ui.Show(*popup);
    const DrawList &both = Frame(ui);
    CHECK(DrawsColor(both, kFirstColor));
    CHECK(DrawsColor(both, kSecondColor));
}

TEST_CASE("Screens: what is shown decides whether the UI takes input and stops the world")
{
    Ui ui;
    Screen *hud = Build(ui, ScreenKind::Persistent, kSortHud, "hud", kFirstColor, "Health");
    Screen *menu = Build(ui, ScreenKind::Stacked, kSortMenu, "menu", kSecondColor, "Resume");

    CHECK_FALSE(ui.TakesInput());
    CHECK_FALSE(ui.PausesWorld());

    // A HUD is looked at, not worked, and a game goes on running behind it.
    ui.Show(*hud);
    CHECK_FALSE(ui.TakesInput());
    CHECK_FALSE(ui.PausesWorld());

    ui.Show(*menu);
    CHECK(ui.TakesInput());
    CHECK(ui.PausesWorld());

    ui.Hide(*menu);
    CHECK_FALSE(ui.TakesInput());
    CHECK_FALSE(ui.PausesWorld());
}

TEST_CASE("Screens: showing another screen drops the hover and press the last one had")
{
    Ui ui;
    Screen *first = Build(ui, ScreenKind::Stacked, kSortMenu, "first", kFirstColor, "One");
    Screen *second = Build(ui, ScreenKind::Stacked, kSortMenu, "second", kSecondColor, "Two");
    ui.Show(*first);
    Frame(ui);

    ui.ProcessInput(Pressing(CentreOf(*first, "One")));
    ui.Sync(kViewport);
    REQUIRE(ui.GetInteraction().pressed == first->Find("One"));
    REQUIRE(ui.GetInteraction().hovered == first->Find("One"));

    // The ids named slots in the first screen's tree; the same slots in the
    // second hold different nodes, so none of them may survive the handover.
    ui.Show(*second);
    CHECK_FALSE(ui.GetInteraction().pressed);
    CHECK_FALSE(ui.GetInteraction().hovered);
    CHECK_FALSE(ui.GetInteraction().activated);
}

TEST_CASE("Screens: destroying a screen hands the keys to whatever is left")
{
    Ui ui;
    Screen *pause = Build(ui, ScreenKind::Stacked, kSortMenu, "pause", kFirstColor, "Resume");
    Screen *settings = Build(ui, ScreenKind::Stacked, kSortMenu, "settings", kSecondColor, "Back");
    ui.Show(*pause);
    ui.Show(*settings);
    Frame(ui);
    REQUIRE(ui.InputScreen() == settings);

    ui.DestroyScreen(*settings);
    CHECK(ui.FindScreen("settings") == nullptr);
    CHECK(ui.InputScreen() == pause);
    CHECK(ui.TakesInput());

    // The frame after still runs: nothing is left pointing into what is gone.
    CHECK_NOTHROW(Frame(ui));
}

TEST_CASE("Screens: a screen is found by name, and only while it exists")
{
    Ui ui;
    Screen *menu = Build(ui, ScreenKind::Stacked, kSortMenu, "menu", kFirstColor, "Resume");
    CHECK(ui.FindScreen("menu") == menu);
    CHECK(ui.FindScreen("nothing") == nullptr);

    ui.DestroyScreen(*menu);
    CHECK(ui.FindScreen("menu") == nullptr);
}

TEST_CASE("Screens: with nothing shown the UI draws nothing and takes no input")
{
    Ui ui;
    Screen *menu = Build(ui, ScreenKind::Stacked, kSortMenu, "menu", kFirstColor, "Resume");
    const DrawList &empty = Frame(ui);
    CHECK(empty.Instances().empty());

    const InputResult result = ui.ProcessInput(Pressing({.x = 100.f, .y = 100.f}));
    ui.Sync(kViewport);
    CHECK_FALSE(result.pointerUsed);
    CHECK_FALSE(result.keyboardTaken);

    ui.Show(*menu);
    CHECK_FALSE(Frame(ui).Instances().empty());
}

TEST_CASE("Screens: Accept reaches the screen with the keys, and Back closes it")
{
    Ui ui;
    Screen *menu = Build(ui, ScreenKind::Stacked, kSortMenu, "menu", kFirstColor, "Resume");
    ui.Show(*menu);
    Frame(ui);

    ui.SetFocus(*menu, menu->Find("Resume"));
    ui.ProcessInput(Action(UiAction::Accept));
    ui.Sync(kViewport);
    CHECK(ui.GetInteraction().activated == menu->Find("Resume"));

    ui.ProcessInput(Action(UiAction::Back));
    ui.Sync(kViewport);
    CHECK_FALSE(menu->IsShown());
}
