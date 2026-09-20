/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "TestFontFixture.hpp"

#include <Assisi/Mondrian/Font.hpp>
#include <Assisi/Mondrian/SampleScreen.hpp>
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

/// The centre of @p name's rect on @p screen, as the last frame placed it.
Point CentreOf(const Screen &screen, std::string_view name)
{
    const LayoutNode *node = screen.GetLayout().Get(screen.Find(name));
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

/// A UI with the sample screen shown and laid out once, so input has something
/// to hit.
struct Sample
{
    Assisi::Core::EventQueue events;
    Ui ui{events};
    /// After the Ui, so it is destroyed first: a screen may not outlive the UI
    /// it registered with.
    std::unique_ptr<Screen> held;
    Screen *screen = nullptr;

    Sample()
    {
        held = AddSampleScreen(ui, kPlaceholder);
        screen = held.get();
        ui.Show(*screen);
        Frame(ui, kLandscape);
    }

    InputResult Step(const UiInput &input)
    {
        const InputResult result = ui.ProcessInput(input);
        ui.Sync(kLandscape);
        return result;
    }

    NodeId Named(std::string_view name) const { return screen->Find(name); }
    const Interaction &Now() const { return ui.GetInteraction(); }
};

bool IsKind(const QuadInstance &quad, QuadKind kind)
{
    return quad.kind == static_cast<uint32_t>(kind);
}

/// The sample panel's rect after a frame at @p viewport.
Rect PanelAt(Sample &sample, Extent viewport)
{
    Frame(sample.ui, viewport);
    const LayoutNode *panel = sample.screen->GetLayout().Get(sample.Named("panel"));
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
        Sample sample;
        sample.ui.SetFont(&font, kFontTexture);
        const DrawList &drawn = Frame(sample.ui, viewport);

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
    Sample sample;
    sample.ui.SetFont(&font, kFontTexture);
    const Rect landscape = PanelAt(sample, kLandscape);
    const Rect portrait = PanelAt(sample, kPortrait);
    CHECK(landscape.width != portrait.width);
    CHECK(VisibleWithin(portrait, kPortrait));
}

TEST_CASE("Mondrian: the player's UI scale enlarges the screen")
{
    const Font font = FixtureFont();
    Sample sample;
    sample.ui.SetFont(&font, kFontTexture);
    const Rect normal = PanelAt(sample, kLandscape);
    sample.ui.SetUserScale(1.5f);
    CHECK(sample.ui.GetUserScale() == 1.5f);
    const Rect larger = PanelAt(sample, kLandscape);
    CHECK(larger.height > normal.height);
}

TEST_CASE("Mondrian: without a font the screen draws no text")
{
    Sample sample;
    const DrawList &drawn = Frame(sample.ui, kLandscape);
    CHECK_FALSE(
        std::ranges::any_of(drawn.Instances(), [](const QuadInstance &quad) { return IsKind(quad, QuadKind::Glyph); }));
    CHECK_FALSE(drawn.Instances().empty());
}

TEST_CASE("Mondrian: the clipboard reads and writes through what the host supplied, and is inert without it")
{
    Assisi::Core::EventQueue events;
    Ui ui{events};
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
    Sample sample;
    sample.ui.SetFont(&font, kFontTexture);
    CHECK(Frame(sample.ui, Extent{0, 0}).Instances().empty());
}

TEST_CASE("Mondrian: the sample screen's list scrolls its content and keeps it inside itself")
{
    // With the font, since what overflows the list is the height of its text.
    const Font font = FixtureFont();
    Sample sample;
    sample.ui.SetFont(&font, kFontTexture);
    sample.Step({});
    const NodeId list = sample.Named("list");
    REQUIRE(list);
    const LayoutNode *placed = sample.screen->GetLayout().Get(list);
    REQUIRE(placed != nullptr);
    CHECK(placed->contentSize.y > placed->rect.height); // there is something to scroll

    UiInput wheel = PointerAt({.x = placed->rect.x + 4.f, .y = placed->rect.y + 4.f}, InputGrant::Pointer);
    wheel.wheel = {.x = 0.f, .y = -1.f};
    CHECK(sample.Step(wheel).wheelUsed);
    CHECK(sample.screen->Tree().Get(list)->scrollTarget.y > 0.f);

    // It glides there rather than jumping, so it arrives a moment later.
    UiInput settling = PointerAt({.x = placed->rect.x + 4.f, .y = placed->rect.y + 4.f}, InputGrant::Pointer);
    settling.time = 1.0;
    sample.Step(settling);
    CHECK(sample.screen->Tree().Get(list)->scrollOffset.y ==
          doctest::Approx(sample.screen->Tree().Get(list)->scrollTarget.y));
    CHECK(sample.screen->Tree().Get(list)->scrollOffset.y > 0.f);

    // Its content is clipped to it, so a scrolled list does not spill over the
    // panel it sits in.
    const LayoutNode *entry = sample.screen->GetLayout().Get(sample.Named("Five"));
    REQUIRE(entry != nullptr);
    CHECK(entry->clip.y >= placed->rect.y);
    CHECK(entry->clip.y + entry->clip.height <= placed->rect.y + placed->rect.height);
}

TEST_CASE("Mondrian: input before any layout hits nothing and does not assert")
{
    Assisi::Core::EventQueue events;
    Ui ui{events};
    const std::unique_ptr<Screen> screen = AddSampleScreen(ui, kPlaceholder);
    ui.Show(*screen);
    const InputResult result = ui.ProcessInput(Pressing({.x = 640.f, .y = 360.f}));
    CHECK_FALSE(result.pointerUsed);
    CHECK_FALSE(ui.GetInteraction().pressed);
}

TEST_CASE("Mondrian: a press and release on one button activates it, and the press is the UI's")
{
    Sample sample;
    const Point resume = CentreOf(*sample.screen, "Resume");
    const InputResult pressed = sample.Step(Pressing(resume));
    CHECK(pressed.pointerUsed);
    CHECK(sample.Now().pressed == sample.Named("Resume"));
    CHECK_FALSE(sample.Now().activated);

    const InputResult released = sample.Step(Releasing(resume));
    CHECK(released.pointerUsed);
    CHECK(sample.Now().activated == sample.Named("Resume"));
    CHECK_FALSE(sample.Now().pressed);

    sample.Step(PointerAt(resume));
    CHECK_FALSE(sample.Now().activated); // for one frame only
}

TEST_CASE("Mondrian: a press keeps its button while the pointer leaves, and a release elsewhere does nothing")
{
    Sample sample;
    sample.Step(Pressing(CentreOf(*sample.screen, "Resume")));

    UiInput held = PointerAt(CentreOf(*sample.screen, "Quit"));
    held.primaryDown = true;
    sample.Step(held);
    CHECK(sample.Now().pressed == sample.Named("Resume"));
    CHECK(sample.Now().hovered == sample.Named("Quit"));

    sample.Step(Releasing(CentreOf(*sample.screen, "Quit")));
    CHECK_FALSE(sample.Now().activated);
}

TEST_CASE("Mondrian: a press on the panel is the UI's, and one beside it is the game's")
{
    Sample sample;
    const LayoutNode *panel = sample.screen->GetLayout().Get(sample.Named("panel"));
    REQUIRE(panel != nullptr);
    const Point inside{.x = panel->rect.x + 4.f, .y = panel->rect.y + 4.f};

    CHECK(sample.Step(Pressing(inside, InputGrant::Pointer)).pointerUsed);
    CHECK_FALSE(sample.Now().pressed); // stopped, but nothing to press
    sample.Step(Releasing(inside, InputGrant::Pointer));

    CHECK_FALSE(sample.Step(Pressing({.x = 1.f, .y = 1.f}, InputGrant::Pointer)).pointerUsed);
}

TEST_CASE("Mondrian: a click on the game clears focus when the game has the keyboard")
{
    Sample sample;
    const Point quit = CentreOf(*sample.screen, "Quit");
    sample.Step(Pressing(quit, InputGrant::Pointer));
    sample.Step(Releasing(quit, InputGrant::Pointer));
    CHECK(sample.Now().focused == sample.Named("Quit"));

    sample.Step(Pressing({.x = 1.f, .y = 1.f}, InputGrant::Pointer));
    CHECK_FALSE(sample.Now().focused);
}

TEST_CASE("Mondrian: a pointer claimed from above hovers nothing and drops a press in progress")
{
    Sample sample;
    const Point resume = CentreOf(*sample.screen, "Resume");
    sample.Step(Pressing(resume));
    REQUIRE(sample.Now().pressed == sample.Named("Resume"));

    UiInput claimed = PointerAt(resume);
    claimed.primaryDown = true;
    claimed.pointerClaimed = true;
    const InputResult result = sample.Step(claimed);
    CHECK_FALSE(result.pointerUsed);
    CHECK_FALSE(sample.Now().hovered);
    CHECK_FALSE(sample.Now().pressed);
}

TEST_CASE("Mondrian: given nothing, the UI reacts to nothing")
{
    Sample sample;
    const InputResult result = sample.Step(Pressing(CentreOf(*sample.screen, "Resume"), InputGrant::Nothing));
    CHECK_FALSE(result.pointerUsed);
    CHECK_FALSE(sample.Now().hovered);
    CHECK_FALSE(sample.Now().pressed);
}

TEST_CASE("Mondrian: with everything, the first move lands on the first button, then moves across and accepts")
{
    Sample sample;
    sample.Step(Action(UiAction::Accept));
    CHECK_FALSE(sample.Now().activated); // nothing focused yet

    sample.Step(Action(UiAction::Right));
    CHECK(sample.Now().device == InputDevice::Keys);
    CHECK(sample.Now().focused == sample.Named("Quit"));

    sample.Step(Action(UiAction::Right));
    CHECK(sample.Now().focused == sample.Named("Resume"));

    sample.Step(Action(UiAction::Accept));
    CHECK(sample.Now().activated == sample.Named("Resume"));

    // Resume is the last thing to its right, so wrapping comes round to
    // something else on the screen.
    sample.Step(Action(UiAction::Right));
    CHECK(sample.Now().focused != sample.Named("Resume"));

    sample.ui.SetNavWrap(NavWrap::Stop);
    sample.ui.SetFocus(*sample.screen, sample.Named("Resume"));
    sample.Step(Action(UiAction::Right));
    CHECK(sample.Now().focused == sample.Named("Resume"));

    // Back last, because it closes the screen everything above acts on.
    const InputResult back = sample.Step(Action(UiAction::Back));
    CHECK(back.keyboardTaken);
    CHECK_FALSE(sample.screen->IsShown());
}

TEST_CASE("Mondrian: with only the pointer, the keys are the game's")
{
    Sample sample;
    const InputResult result = sample.Step(Action(UiAction::Right, 0.0, InputGrant::Pointer));
    CHECK_FALSE(result.keyboardTaken);
    CHECK_FALSE(sample.Now().focused);
    CHECK(sample.Now().device == InputDevice::Pointer);
}

TEST_CASE("Mondrian: a focused node that takes the keyboard has it even while the game has it")
{
    Sample sample;
    const NodeId resume = sample.Named("Resume");
    sample.screen->Tree().SetTakesKeyboard(resume, true);
    sample.ui.SetFocus(*sample.screen, resume);

    const InputResult result = sample.Step(Action(UiAction::Accept, 0.0, InputGrant::Pointer));
    CHECK(result.keyboardTaken);
    CHECK(sample.Now().activated == resume);

    UiInput claimed = Action(UiAction::Accept, 0.0, InputGrant::Pointer);
    claimed.keyboardClaimed = true;
    CHECK_FALSE(sample.Step(claimed).keyboardTaken);
}

TEST_CASE("Mondrian: a held direction moves once, then repeats after a delay at an interval")
{
    Sample sample;
    sample.Step(Action(UiAction::Right, 0.0)); // focus arrives on Quit
    const auto focused = [&sample] { return sample.Now().focused; };
    const NodeId start = focused();

    sample.Step(Holding(UiAction::Right, kNavRepeatDelaySeconds / 2.0));
    CHECK(focused() == start);

    sample.Step(Holding(UiAction::Right, kNavRepeatDelaySeconds));
    CHECK(focused() != start);
    const NodeId repeated = focused();

    sample.Step(Holding(UiAction::Right, kNavRepeatDelaySeconds + (kNavRepeatIntervalSeconds / 2.0)));
    CHECK(focused() == repeated);
    sample.Step(Holding(UiAction::Right, kNavRepeatDelaySeconds + kNavRepeatIntervalSeconds));
    CHECK(focused() != repeated);

    UiInput released;
    released.grant = InputGrant::Everything;
    released.time = 10.0;
    const NodeId stopped = focused();
    sample.Step(released);
    sample.Step(Holding(UiAction::Right, 20.0)); // held again without a press: no repeat carried over
    CHECK(focused() == stopped);
}

TEST_CASE("Mondrian: hovering focuses only when the developer asks, and never while keys were used last")
{
    Sample sample;
    const Point quit = CentreOf(*sample.screen, "Quit");
    sample.Step(PointerAt(quit));
    CHECK(sample.Now().hovered == sample.Named("Quit"));
    CHECK_FALSE(sample.Now().focused);

    sample.ui.SetHoverFocuses(true);
    sample.Step(PointerAt({.x = quit.x + 1.f, .y = quit.y}));
    CHECK(sample.Now().focused == sample.Named("Quit"));

    UiInput keys = Action(UiAction::Right);
    keys.pointer = {.x = quit.x + 1.f, .y = quit.y};
    sample.Step(keys);
    CHECK(sample.Now().focused == sample.Named("Resume"));

    sample.Step(PointerAt({.x = quit.x + 1.f, .y = quit.y})); // still over Quit, but it did not move
    CHECK(sample.Now().device == InputDevice::Keys);
    CHECK(sample.Now().focused == sample.Named("Resume"));

    sample.Step(PointerAt(quit));
    CHECK(sample.Now().device == InputDevice::Pointer);
    CHECK(sample.Now().focused == sample.Named("Quit"));
}

TEST_CASE("Mondrian: focus set before the first layout survives it")
{
    Assisi::Core::EventQueue events;
    Ui ui{events};
    const std::unique_ptr<Screen> screen = AddSampleScreen(ui, kPlaceholder);
    ui.Show(*screen);
    ui.SetFocus(*screen, screen->Find("Resume"));
    Frame(ui, kLandscape);
    Frame(ui, kLandscape);
    CHECK(ui.GetInteraction().focused == screen->Find("Resume"));
}

TEST_CASE("Mondrian: focus on a node that is gone falls back to the first that can take it")
{
    Sample sample;
    sample.ui.SetFocus(*sample.screen, sample.Named("Resume"));
    sample.screen->Tree().Destroy(sample.Named("Resume"));
    sample.Step(PointerAt({}));
    CHECK(sample.Now().focused == sample.Named("Quit"));
}

TEST_CASE("Mondrian: the focus ring shows while keys were used last, and not while pointing")
{
    Sample sample;
    sample.Step(Action(UiAction::Next));
    const std::size_t withRing = sample.ui.GetDrawList().Instances().size();

    sample.Step(PointerAt({.x = 5.f, .y = 5.f}));
    const std::size_t withoutRing = sample.ui.GetDrawList().Instances().size();
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
    Sample sample;
    Assisi::Core::EventQueue &events = sample.events;
    sample.screen->OnActivate(sample.Named("Resume"), ResumeClicked{});

    const Point resume = CentreOf(*sample.screen, "Resume");
    sample.Step(Pressing(resume, InputGrant::Pointer));
    CHECK(events.Read<ResumeClicked>().empty()); // a press alone is not a click

    sample.Step(Releasing(resume, InputGrant::Pointer));
    CHECK(events.Read<ResumeClicked>().size() == 1);

    sample.Step(PointerAt(resume, InputGrant::Pointer));
    CHECK(events.Read<ResumeClicked>().size() == 1); // nothing more without another click
}

TEST_CASE("Mondrian: accepting a focused button pushes the event it carries")
{
    Sample sample;
    Assisi::Core::EventQueue &events = sample.events;
    constexpr int32_t kCode = 7;
    sample.screen->OnActivate(sample.Named("Quit"), QuitClicked{.code = kCode});

    sample.Step(Action(UiAction::Next)); // focus lands on Quit
    sample.Step(Action(UiAction::Accept));

    REQUIRE(events.Read<QuitClicked>().size() == 1);
    CHECK(events.Read<QuitClicked>()[0].code == kCode);
    CHECK(events.Read<ResumeClicked>().empty());
}

TEST_CASE("Mondrian: binding a button again replaces what it pushes")
{
    Sample sample;
    Assisi::Core::EventQueue &events = sample.events;
    sample.screen->OnActivate(sample.Named("Resume"), ResumeClicked{});
    sample.screen->OnActivate(sample.Named("Resume"), QuitClicked{});

    const Point resume = CentreOf(*sample.screen, "Resume");
    sample.Step(Pressing(resume));
    sample.Step(Releasing(resume));
    CHECK(events.Read<ResumeClicked>().empty());
    CHECK(events.Read<QuitClicked>().size() == 1);
}

TEST_CASE("Mondrian: Back closes the screen only while the UI has the keys")
{
    Sample sample;

    sample.Step(Action(UiAction::Back, 0.0, InputGrant::Pointer));
    CHECK(sample.screen->IsShown());

    sample.Step(Action(UiAction::Back));
    CHECK_FALSE(sample.screen->IsShown());
}

TEST_CASE("Mondrian: with no event queue, activating a bound button does nothing and does not assert")
{
    Sample sample;
    sample.screen->OnActivate(sample.Named("Resume"), ResumeClicked{});
    const Point resume = CentreOf(*sample.screen, "Resume");
    sample.Step(Pressing(resume));
    CHECK_NOTHROW(sample.Step(Releasing(resume)));
    CHECK(sample.Now().activated == sample.Named("Resume"));
}

#ifndef NDEBUG
TEST_CASE("Mondrian: the two frame steps must alternate, input first")
{
    const Assisi::Testing::ThrowOnContractViolation guard;

    SUBCASE("sync before any input asserts")
    {
        Assisi::Core::EventQueue events;
        Ui ui{events};
        CHECK_THROWS_AS(ui.Sync(kLandscape), Assisi::Core::ContractViolation);
    }

    SUBCASE("input twice without a sync asserts")
    {
        Assisi::Core::EventQueue events;
        Ui ui{events};
        ui.ProcessInput({});
        CHECK_THROWS_AS(ui.ProcessInput({}), Assisi::Core::ContractViolation);
    }

    SUBCASE("sync twice without input asserts")
    {
        Assisi::Core::EventQueue events;
        Ui ui{events};
        ui.ProcessInput({});
        ui.Sync(kLandscape);
        CHECK_THROWS_AS(ui.Sync(kLandscape), Assisi::Core::ContractViolation);
    }

    SUBCASE("alternating frames are accepted")
    {
        Assisi::Core::EventQueue events;
        Ui ui{events};
        CHECK_NOTHROW(Frame(ui, kLandscape));
        CHECK_NOTHROW(Frame(ui, kPortrait));
    }
}
#endif
