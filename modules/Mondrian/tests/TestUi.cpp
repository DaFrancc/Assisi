/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "TestFontFixture.hpp"

#include <Assisi/Mondrian/Font.hpp>
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Testing/ThrowOnContractViolation.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

using namespace Assisi::Mondrian;
using Assisi::Mondrian::Testing::FixtureFont;

namespace
{

/// A frame of each shape the UI must handle: a common window, and one narrower
/// than it is tall so a layout that mixes up the axes shows it.
constexpr Extent kLandscape{1280, 720};
constexpr Extent kPortrait{480, 960};

constexpr TextureId kPlaceholder{7};
constexpr TextureId kFontTexture{9};

/// Whether @p rect covers some pixels and all of them lie inside @p viewport.
bool VisibleWithin(const Rect &rect, Extent viewport)
{
    return rect.width > 0.f && rect.height > 0.f && rect.x >= 0.f && rect.y >= 0.f &&
           rect.x + rect.width <= static_cast<float>(viewport.width) &&
           rect.y + rect.height <= static_cast<float>(viewport.height);
}

/// Runs one whole frame with no input and returns what it drew.
const DrawList &Frame(Ui &ui, Extent viewport)
{
    ui.ProcessInput({});
    ui.Sync(viewport);
    return ui.GetDrawList();
}

/// The centre of @p name's rect as the last frame placed it.
Point CentreOf(const Ui &ui, std::string_view name)
{
    const LayoutNode *node = ui.GetLayout().Get(ui.Tree().Find(name));
    REQUIRE(node != nullptr);
    return {.x = node->rect.x + (node->rect.width / 2.f), .y = node->rect.y + (node->rect.height / 2.f)};
}

/// Input with the pointer at @p pointer, given what @p grant says.
UiInput PointerAt(Point pointer, InputGrant grant = InputGrant::Everything)
{
    UiInput input;
    input.pointer = pointer;
    input.grant = grant;
    return input;
}

UiInput Pressing(Point pointer, InputGrant grant = InputGrant::Everything)
{
    UiInput input = PointerAt(pointer, grant);
    input.primaryDown = true;
    input.primaryPressed = true;
    return input;
}

UiInput Releasing(Point pointer, InputGrant grant = InputGrant::Everything)
{
    UiInput input = PointerAt(pointer, grant);
    input.primaryReleased = true;
    return input;
}

/// Input pressing @p action at @p time, with the pointer where @p pointer is.
UiInput Action(UiAction action, double time = 0.0, InputGrant grant = InputGrant::Everything)
{
    UiInput input;
    input.time = time;
    input.grant = grant;
    input.actionPressed[static_cast<std::size_t>(action)] = true;
    input.actionDown[static_cast<std::size_t>(action)] = true;
    return input;
}

UiInput Holding(UiAction action, double time)
{
    UiInput input = Action(action, time);
    input.actionPressed = {};
    return input;
}

/// A UI with the sample screen laid out once, so input has something to hit.
struct Screen
{
    Ui ui;

    Screen() { Frame(ui, kLandscape); }

    InputResult Step(const UiInput &input)
    {
        const InputResult result = ui.ProcessInput(input);
        ui.Sync(kLandscape);
        return result;
    }

    NodeId Named(std::string_view name) const { return ui.Tree().Find(name); }
    const Interaction &Now() const { return ui.GetInteraction(); }
};

bool IsKind(const QuadInstance &quad, QuadKind kind)
{
    return quad.kind == static_cast<uint32_t>(kind);
}

/// The sample panel's rect after a frame at @p viewport.
Rect PanelAt(Ui &ui, Extent viewport)
{
    Frame(ui, viewport);
    const LayoutNode *panel = ui.GetLayout().Get(ui.Tree().Find("panel"));
    REQUIRE(panel != nullptr);
    return panel->rect;
}

} // namespace

TEST_CASE("Mondrian: the sample screen draws boxes, an image and text inside the viewport")
{
    const Font font = FixtureFont();
    for (const Extent viewport : {kLandscape, kPortrait})
    {
        CAPTURE(viewport.width);
        Ui ui;
        ui.SetFont(&font, kFontTexture);
        ui.SetPlaceholderTexture(kPlaceholder);
        const DrawList &drawn = Frame(ui, viewport);

        REQUIRE(drawn.IsFinalized());
        const auto quads = drawn.Instances();
        CHECK(std::ranges::any_of(quads, [](const QuadInstance &quad) { return IsKind(quad, QuadKind::Solid); }));
        CHECK(std::ranges::any_of(quads, [](const QuadInstance &quad) { return IsKind(quad, QuadKind::Image); }));
        CHECK(std::ranges::any_of(quads, [](const QuadInstance &quad) { return IsKind(quad, QuadKind::Glyph); }));
        for (const QuadInstance &quad : quads)
        {
            if (!IsKind(quad, QuadKind::Glyph))
            {
                CHECK(VisibleWithin(quad.rect, viewport));
            }
        }
        CHECK(
            std::ranges::any_of(drawn.Entries(), [](const DrawEntry &entry) { return entry.texture == kFontTexture; }));
        CHECK(
            std::ranges::any_of(drawn.Entries(), [](const DrawEntry &entry) { return entry.texture == kPlaceholder; }));
    }
}

TEST_CASE("Mondrian: the sample screen reflows when the viewport changes")
{
    const Font font = FixtureFont();
    Ui ui;
    ui.SetFont(&font, kFontTexture);
    const Rect landscape = PanelAt(ui, kLandscape);
    const Rect portrait = PanelAt(ui, kPortrait);
    CHECK(landscape.width != portrait.width);
    CHECK(VisibleWithin(portrait, kPortrait));
}

TEST_CASE("Mondrian: the player's UI scale enlarges the screen")
{
    const Font font = FixtureFont();
    Ui ui;
    ui.SetFont(&font, kFontTexture);
    const Rect normal = PanelAt(ui, kLandscape);
    ui.SetUserScale(1.5f);
    CHECK(ui.GetUserScale() == 1.5f);
    const Rect larger = PanelAt(ui, kLandscape);
    CHECK(larger.height > normal.height);
}

TEST_CASE("Mondrian: without a font the screen draws no text")
{
    Ui ui;
    const DrawList &drawn = Frame(ui, kLandscape);
    CHECK_FALSE(
        std::ranges::any_of(drawn.Instances(), [](const QuadInstance &quad) { return IsKind(quad, QuadKind::Glyph); }));
    CHECK_FALSE(drawn.Instances().empty());
}

TEST_CASE("Mondrian: the clipboard reads and writes through what the host supplied, and is inert without it")
{
    Ui ui;
    CHECK(ui.GetClipboard().Read().empty());
    ui.GetClipboard().Write("dropped");

    std::string held = "caf\xC3\xA9";
    ui.SetClipboard(
        Clipboard{.read = [&held] { return held; }, .write = [&held](std::string_view text) { held = text; }});
    CHECK(ui.GetClipboard().Read() == "caf\xC3\xA9");
    ui.GetClipboard().Write("copied");
    CHECK(held == "copied");
}

TEST_CASE("Mondrian: a zero-sized viewport draws nothing and does not assert")
{
    const Font font = FixtureFont();
    Ui ui;
    ui.SetFont(&font, kFontTexture);
    CHECK(Frame(ui, Extent{0, 0}).Instances().empty());
}

TEST_CASE("Mondrian: the sample screen's list scrolls its content and keeps it inside itself")
{
    // With the font, since what overflows the list is the height of its text.
    const Font font = FixtureFont();
    Screen screen;
    screen.ui.SetFont(&font, kFontTexture);
    screen.Step({});
    const NodeId list = screen.Named("list");
    REQUIRE(list);
    const LayoutNode *placed = screen.ui.GetLayout().Get(list);
    REQUIRE(placed != nullptr);
    CHECK(placed->contentSize.y > placed->rect.height); // there is something to scroll

    UiInput wheel = PointerAt({.x = placed->rect.x + 4.f, .y = placed->rect.y + 4.f}, InputGrant::Pointer);
    wheel.wheel = {.x = 0.f, .y = -1.f};
    CHECK(screen.Step(wheel).wheelUsed);
    CHECK(screen.ui.Tree().Get(list)->scrollTarget.y > 0.f);

    // It glides there rather than jumping, so it arrives a moment later.
    UiInput settling = PointerAt({.x = placed->rect.x + 4.f, .y = placed->rect.y + 4.f}, InputGrant::Pointer);
    settling.time = 1.0;
    screen.Step(settling);
    CHECK(screen.ui.Tree().Get(list)->scrollOffset.y == doctest::Approx(screen.ui.Tree().Get(list)->scrollTarget.y));
    CHECK(screen.ui.Tree().Get(list)->scrollOffset.y > 0.f);

    // Its content is clipped to it, so a scrolled list does not spill over the
    // panel it sits in.
    const LayoutNode *entry = screen.ui.GetLayout().Get(screen.Named("Five"));
    REQUIRE(entry != nullptr);
    CHECK(entry->clip.y >= placed->rect.y);
    CHECK(entry->clip.y + entry->clip.height <= placed->rect.y + placed->rect.height);
}

TEST_CASE("Mondrian: input before any layout hits nothing and does not assert")
{
    Ui ui;
    const InputResult result = ui.ProcessInput(Pressing({.x = 640.f, .y = 360.f}));
    CHECK_FALSE(result.pointerUsed);
    CHECK_FALSE(ui.GetInteraction().pressed);
}

TEST_CASE("Mondrian: a press and release on one button activates it, and the press is the UI's")
{
    Screen screen;
    const Point resume = CentreOf(screen.ui, "Resume");
    const InputResult pressed = screen.Step(Pressing(resume));
    CHECK(pressed.pointerUsed);
    CHECK(screen.Now().pressed == screen.Named("Resume"));
    CHECK_FALSE(screen.Now().activated);

    const InputResult released = screen.Step(Releasing(resume));
    CHECK(released.pointerUsed);
    CHECK(screen.Now().activated == screen.Named("Resume"));
    CHECK_FALSE(screen.Now().pressed);

    screen.Step(PointerAt(resume));
    CHECK_FALSE(screen.Now().activated); // for one frame only
}

TEST_CASE("Mondrian: a press keeps its button while the pointer leaves, and a release elsewhere does nothing")
{
    Screen screen;
    screen.Step(Pressing(CentreOf(screen.ui, "Resume")));

    UiInput held = PointerAt(CentreOf(screen.ui, "Quit"));
    held.primaryDown = true;
    screen.Step(held);
    CHECK(screen.Now().pressed == screen.Named("Resume"));
    CHECK(screen.Now().hovered == screen.Named("Quit"));

    screen.Step(Releasing(CentreOf(screen.ui, "Quit")));
    CHECK_FALSE(screen.Now().activated);
}

TEST_CASE("Mondrian: a press on the panel is the UI's, and one beside it is the game's")
{
    Screen screen;
    const LayoutNode *panel = screen.ui.GetLayout().Get(screen.Named("panel"));
    REQUIRE(panel != nullptr);
    const Point inside{.x = panel->rect.x + 4.f, .y = panel->rect.y + 4.f};

    CHECK(screen.Step(Pressing(inside, InputGrant::Pointer)).pointerUsed);
    CHECK_FALSE(screen.Now().pressed); // stopped, but nothing to press
    screen.Step(Releasing(inside, InputGrant::Pointer));

    CHECK_FALSE(screen.Step(Pressing({.x = 1.f, .y = 1.f}, InputGrant::Pointer)).pointerUsed);
}

TEST_CASE("Mondrian: a click on the game clears focus when the game has the keyboard")
{
    Screen screen;
    const Point quit = CentreOf(screen.ui, "Quit");
    screen.Step(Pressing(quit, InputGrant::Pointer));
    screen.Step(Releasing(quit, InputGrant::Pointer));
    CHECK(screen.Now().focused == screen.Named("Quit"));

    screen.Step(Pressing({.x = 1.f, .y = 1.f}, InputGrant::Pointer));
    CHECK_FALSE(screen.Now().focused);
}

TEST_CASE("Mondrian: a pointer claimed from above hovers nothing and drops a press in progress")
{
    Screen screen;
    const Point resume = CentreOf(screen.ui, "Resume");
    screen.Step(Pressing(resume));
    REQUIRE(screen.Now().pressed == screen.Named("Resume"));

    UiInput claimed = PointerAt(resume);
    claimed.primaryDown = true;
    claimed.pointerClaimed = true;
    const InputResult result = screen.Step(claimed);
    CHECK_FALSE(result.pointerUsed);
    CHECK_FALSE(screen.Now().hovered);
    CHECK_FALSE(screen.Now().pressed);
}

TEST_CASE("Mondrian: given nothing, the UI reacts to nothing")
{
    Screen screen;
    const InputResult result = screen.Step(Pressing(CentreOf(screen.ui, "Resume"), InputGrant::Nothing));
    CHECK_FALSE(result.pointerUsed);
    CHECK_FALSE(screen.Now().hovered);
    CHECK_FALSE(screen.Now().pressed);
}

TEST_CASE("Mondrian: with everything, the first move lands on the first button, then moves across and accepts")
{
    Screen screen;
    screen.Step(Action(UiAction::Accept));
    CHECK_FALSE(screen.Now().activated); // nothing focused yet

    screen.Step(Action(UiAction::Right));
    CHECK(screen.Now().device == InputDevice::Keys);
    CHECK(screen.Now().focused == screen.Named("Quit"));

    screen.Step(Action(UiAction::Right));
    CHECK(screen.Now().focused == screen.Named("Resume"));

    screen.Step(Action(UiAction::Accept));
    CHECK(screen.Now().activated == screen.Named("Resume"));

    const InputResult back = screen.Step(Action(UiAction::Back));
    CHECK(screen.Now().backPressed);
    CHECK(back.keyboardTaken);

    // Resume is the last thing to its right, so wrapping comes round to
    // something else on the screen.
    screen.Step(Action(UiAction::Right));
    CHECK(screen.Now().focused != screen.Named("Resume"));

    screen.ui.SetNavWrap(NavWrap::Stop);
    screen.ui.SetFocus(screen.Named("Resume"));
    screen.Step(Action(UiAction::Right));
    CHECK(screen.Now().focused == screen.Named("Resume"));
}

TEST_CASE("Mondrian: with only the pointer, the keys are the game's")
{
    Screen screen;
    const InputResult result = screen.Step(Action(UiAction::Right, 0.0, InputGrant::Pointer));
    CHECK_FALSE(result.keyboardTaken);
    CHECK_FALSE(screen.Now().focused);
    CHECK(screen.Now().device == InputDevice::Pointer);
}

TEST_CASE("Mondrian: a focused node that takes the keyboard has it even while the game has it")
{
    Screen screen;
    const NodeId resume = screen.Named("Resume");
    screen.ui.Tree().SetTakesKeyboard(resume, true);
    screen.ui.SetFocus(resume);

    const InputResult result = screen.Step(Action(UiAction::Accept, 0.0, InputGrant::Pointer));
    CHECK(result.keyboardTaken);
    CHECK(screen.Now().activated == resume);

    UiInput claimed = Action(UiAction::Accept, 0.0, InputGrant::Pointer);
    claimed.keyboardClaimed = true;
    CHECK_FALSE(screen.Step(claimed).keyboardTaken);
}

TEST_CASE("Mondrian: a held direction moves once, then repeats after a delay at an interval")
{
    Screen screen;
    screen.Step(Action(UiAction::Right, 0.0)); // focus arrives on Quit
    const auto focused = [&screen] { return screen.Now().focused; };
    const NodeId start = focused();

    screen.Step(Holding(UiAction::Right, kNavRepeatDelaySeconds / 2.0));
    CHECK(focused() == start);

    screen.Step(Holding(UiAction::Right, kNavRepeatDelaySeconds));
    CHECK(focused() != start);
    const NodeId repeated = focused();

    screen.Step(Holding(UiAction::Right, kNavRepeatDelaySeconds + (kNavRepeatIntervalSeconds / 2.0)));
    CHECK(focused() == repeated);
    screen.Step(Holding(UiAction::Right, kNavRepeatDelaySeconds + kNavRepeatIntervalSeconds));
    CHECK(focused() != repeated);

    UiInput released;
    released.grant = InputGrant::Everything;
    released.time = 10.0;
    const NodeId stopped = focused();
    screen.Step(released);
    screen.Step(Holding(UiAction::Right, 20.0)); // held again without a press: no repeat carried over
    CHECK(focused() == stopped);
}

TEST_CASE("Mondrian: hovering focuses only when the developer asks, and never while keys were used last")
{
    Screen screen;
    const Point quit = CentreOf(screen.ui, "Quit");
    screen.Step(PointerAt(quit));
    CHECK(screen.Now().hovered == screen.Named("Quit"));
    CHECK_FALSE(screen.Now().focused);

    screen.ui.SetHoverFocuses(true);
    screen.Step(PointerAt({.x = quit.x + 1.f, .y = quit.y}));
    CHECK(screen.Now().focused == screen.Named("Quit"));

    UiInput keys = Action(UiAction::Right);
    keys.pointer = {.x = quit.x + 1.f, .y = quit.y};
    screen.Step(keys);
    CHECK(screen.Now().focused == screen.Named("Resume"));

    screen.Step(PointerAt({.x = quit.x + 1.f, .y = quit.y})); // still over Quit, but it did not move
    CHECK(screen.Now().device == InputDevice::Keys);
    CHECK(screen.Now().focused == screen.Named("Resume"));

    screen.Step(PointerAt(quit));
    CHECK(screen.Now().device == InputDevice::Pointer);
    CHECK(screen.Now().focused == screen.Named("Quit"));
}

TEST_CASE("Mondrian: focus set before the first layout survives it")
{
    Ui ui;
    ui.SetFocus(ui.Tree().Find("Resume"));
    Frame(ui, kLandscape);
    Frame(ui, kLandscape);
    CHECK(ui.GetInteraction().focused == ui.Tree().Find("Resume"));
}

TEST_CASE("Mondrian: focus on a node that is gone falls back to the first that can take it")
{
    Screen screen;
    screen.ui.SetFocus(screen.Named("Resume"));
    screen.ui.Tree().Destroy(screen.Named("Resume"));
    screen.Step(PointerAt({}));
    CHECK(screen.Now().focused == screen.Named("Quit"));
}

TEST_CASE("Mondrian: the focus ring shows while keys were used last, and not while pointing")
{
    Screen screen;
    screen.Step(Action(UiAction::Next));
    const std::size_t withRing = screen.ui.GetDrawList().Instances().size();

    screen.Step(PointerAt({.x = 5.f, .y = 5.f}));
    const std::size_t withoutRing = screen.ui.GetDrawList().Instances().size();
    CHECK(withRing == withoutRing + 1);
}

namespace
{

struct ResumeClicked
{
};

struct QuitClicked
{
    int32_t code = 0;
};

} // namespace

TEST_CASE("Mondrian: a click on a bound button pushes its event once")
{
    Screen screen;
    Assisi::Core::EventQueue events;
    screen.ui.SetEvents(&events);
    screen.ui.OnActivate(screen.Named("Resume"), ResumeClicked{});

    const Point resume = CentreOf(screen.ui, "Resume");
    screen.Step(Pressing(resume, InputGrant::Pointer));
    CHECK(events.Read<ResumeClicked>().empty()); // a press alone is not a click

    screen.Step(Releasing(resume, InputGrant::Pointer));
    CHECK(events.Read<ResumeClicked>().size() == 1);

    screen.Step(PointerAt(resume, InputGrant::Pointer));
    CHECK(events.Read<ResumeClicked>().size() == 1); // nothing more without another click
}

TEST_CASE("Mondrian: accepting a focused button pushes the event it carries")
{
    Screen screen;
    Assisi::Core::EventQueue events;
    screen.ui.SetEvents(&events);
    constexpr int32_t kCode = 7;
    screen.ui.OnActivate(screen.Named("Quit"), QuitClicked{.code = kCode});

    screen.Step(Action(UiAction::Next)); // focus lands on Quit
    screen.Step(Action(UiAction::Accept));

    REQUIRE(events.Read<QuitClicked>().size() == 1);
    CHECK(events.Read<QuitClicked>()[0].code == kCode);
    CHECK(events.Read<ResumeClicked>().empty());
}

TEST_CASE("Mondrian: binding a button again replaces what it pushes")
{
    Screen screen;
    Assisi::Core::EventQueue events;
    screen.ui.SetEvents(&events);
    screen.ui.OnActivate(screen.Named("Resume"), ResumeClicked{});
    screen.ui.OnActivate(screen.Named("Resume"), QuitClicked{});

    const Point resume = CentreOf(screen.ui, "Resume");
    screen.Step(Pressing(resume));
    screen.Step(Releasing(resume));
    CHECK(events.Read<ResumeClicked>().empty());
    CHECK(events.Read<QuitClicked>().size() == 1);
}

TEST_CASE("Mondrian: Back pushes UiBack only while the UI has the keys")
{
    Screen screen;
    Assisi::Core::EventQueue events;
    screen.ui.SetEvents(&events);

    screen.Step(Action(UiAction::Back, 0.0, InputGrant::Pointer));
    CHECK(events.Read<UiBack>().empty());

    screen.Step(Action(UiAction::Back));
    CHECK(events.Read<UiBack>().size() == 1);
}

TEST_CASE("Mondrian: with no event queue, activating a bound button does nothing and does not assert")
{
    Screen screen;
    screen.ui.OnActivate(screen.Named("Resume"), ResumeClicked{});
    const Point resume = CentreOf(screen.ui, "Resume");
    screen.Step(Pressing(resume));
    CHECK_NOTHROW(screen.Step(Releasing(resume)));
    CHECK(screen.Now().activated == screen.Named("Resume"));
}

#ifndef NDEBUG
TEST_CASE("Mondrian: the two frame steps must alternate, input first")
{
    const Assisi::Testing::ThrowOnContractViolation guard;

    SUBCASE("sync before any input asserts")
    {
        Ui ui;
        CHECK_THROWS_AS(ui.Sync(kLandscape), Assisi::Core::ContractViolation);
    }

    SUBCASE("input twice without a sync asserts")
    {
        Ui ui;
        ui.ProcessInput({});
        CHECK_THROWS_AS(ui.ProcessInput({}), Assisi::Core::ContractViolation);
    }

    SUBCASE("sync twice without input asserts")
    {
        Ui ui;
        ui.ProcessInput({});
        ui.Sync(kLandscape);
        CHECK_THROWS_AS(ui.Sync(kLandscape), Assisi::Core::ContractViolation);
    }

    SUBCASE("alternating frames are accepted")
    {
        Ui ui;
        CHECK_NOTHROW(Frame(ui, kLandscape));
        CHECK_NOTHROW(Frame(ui, kPortrait));
    }
}
#endif
