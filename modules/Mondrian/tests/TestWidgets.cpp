/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Mondrian/Widget.hpp>

#include <doctest/doctest.h>

#include <cstddef>

using namespace Assisi::Mondrian;

namespace
{

constexpr Extent kScreen{1920, 1080};

/// A range a game would recognise, rather than a bare fraction.
constexpr SliderRange kPercent{.min = 0.f, .max = 100.f, .step = 10.f};
/// Four steps, whose values fall on whole numbers.
constexpr SliderRange kQuality{.min = 0.f, .max = 3.f, .step = 1.f};

struct VolumeChanged
{
    float volume = 0.f;
};

struct SwitchFlipped
{
    bool on = false;
};

struct StepPicked
{
    int32_t step = 0;
};

/// A UI with one control under a row, laid out so input has something to hit.
struct Panel
{
    Assisi::Core::EventQueue events;
    /// After the queue, so the queue outlives it.
    Ui ui{events};
    /// After the Ui, so it is destroyed first: a screen may not outlive the UI
    /// it registered with.
    std::unique_ptr<Screen> held;
    Screen *screen = nullptr;
    NodeId row;

    Panel()
    {
        held = std::make_unique<Screen>(ui,
                                        ScreenTraits{.input = ScreenInput::ConsumeInput,
                                                     .beneath = ScreenBeneath::HidesBeneath,
                                                     .pause = ScreenPause::Pause},
                                        kSortMenu, "panel");
        screen = held.get();
        ui.Show(*screen);

        Style style;
        style.floating.enabled = true;
        row = screen->Tree().Create(screen->Root());
        screen->Tree().SetStyle(row, style);
        Step({});
    }

    InputResult Step(const UiInput &input)
    {
        const InputResult result = ui.ProcessInput(input);
        ui.Sync(kScreen);
        return result;
    }

    [[nodiscard]] Rect RectOf(NodeId node) const
    {
        const LayoutNode *placed = screen->GetLayout().Get(node);
        REQUIRE(placed != nullptr);
        return placed->rect;
    }
};

UiInput At(Point pointer)
{
    UiInput input;
    input.pointer = pointer;
    input.grant = InputGrant::Everything;
    return input;
}

UiInput Pressing(Point pointer)
{
    UiInput input = At(pointer);
    input.primaryDown = true;
    input.primaryPressed = true;
    return input;
}

UiInput Holding(Point pointer)
{
    UiInput input = At(pointer);
    input.primaryDown = true;
    return input;
}

UiInput Releasing(Point pointer)
{
    UiInput input = At(pointer);
    input.primaryReleased = true;
    return input;
}

UiInput Pressing(UiAction action, double time = 0.0)
{
    UiInput input;
    input.time = time;
    input.grant = InputGrant::Everything;
    input.actionPressed[static_cast<std::size_t>(action)] = true;
    input.actionDown[static_cast<std::size_t>(action)] = true;
    return input;
}

UiInput HoldingAction(UiAction action, double time)
{
    UiInput input = Pressing(action, time);
    input.actionPressed = {};
    return input;
}

UiInput Wheeling(Point pointer, float notches)
{
    UiInput input = At(pointer);
    input.wheel = {.x = 0.f, .y = notches};
    return input;
}

Point Middle(const Rect &rect)
{
    return {.x = rect.x + (rect.width / 2.f), .y = rect.y + (rect.height / 2.f)};
}

} // namespace

TEST_CASE("Widgets: a slider follows where it is clicked and tells the game the fraction")
{
    Panel panel;
    const ContinuousSliderId volume = panel.screen->AddContinuousSlider(panel.row, kPercent, 0.f);
    panel.screen->OnChange(volume, [](float value) { return VolumeChanged{value}; });
    panel.Step({});

    const Rect track = panel.RectOf(volume.node);
    panel.Step(Pressing({.x = track.x + (track.width / 2.f), .y = Middle(track).y}));
    REQUIRE(panel.events.Read<VolumeChanged>().size() == 1);
    // The value is in the range the slider was given, not a fraction of it.
    CHECK(panel.events.Read<VolumeChanged>()[0].volume == doctest::Approx(50.f).epsilon(0.05));

    // Held, the slider keeps the pointer: dragging past its end pins it at one.
    panel.Step(Holding({.x = track.x + (2.f * track.width), .y = Middle(track).y}));
    REQUIRE(panel.events.Read<VolumeChanged>().size() == 2);
    CHECK(panel.events.Read<VolumeChanged>()[1].volume == doctest::Approx(kPercent.max));

    // A frame that moves it nowhere says nothing.
    panel.Step(Holding({.x = track.x + (2.f * track.width), .y = Middle(track).y}));
    CHECK(panel.events.Read<VolumeChanged>().size() == 2);
}

TEST_CASE("Widgets: a slider steps on the horizontal keys, which then do not move focus")
{
    Panel panel;
    const ContinuousSliderId volume = panel.screen->AddContinuousSlider(panel.row, kPercent, 50.f);
    const ButtonId beside = panel.screen->AddButton(panel.row, "Beside");
    panel.screen->OnChange(volume, [](float value) { return VolumeChanged{value}; });
    panel.Step({});
    panel.ui.SetFocus(*panel.screen, volume.node);

    panel.Step(Pressing(UiAction::Right));
    REQUIRE(panel.events.Read<VolumeChanged>().size() == 1);
    CHECK(panel.events.Read<VolumeChanged>()[0].volume == doctest::Approx(50.f + kPercent.step));
    CHECK(panel.ui.GetInteraction().focused == volume.node);
    CHECK(panel.ui.GetInteraction().focused != beside.node);

    // Held past the repeat delay, it steps again.
    panel.Step(HoldingAction(UiAction::Right, kNavRepeatDelaySeconds / 2.0));
    CHECK(panel.events.Read<VolumeChanged>().size() == 1);
    panel.Step(HoldingAction(UiAction::Right, kNavRepeatDelaySeconds));
    CHECK(panel.events.Read<VolumeChanged>().size() == 2);
}

TEST_CASE("Widgets: a stepped slider lands on whole steps and counts them out")
{
    Panel panel;
    constexpr int32_t kSteps = 5; // positions 0 to 4
    const SteppedSliderId quality = panel.screen->AddSteppedSlider(panel.row, kQuality, kSteps, 0);
    panel.screen->OnChange(quality, [](int32_t step) { return StepPicked{step}; });
    panel.Step({});

    const Rect track = panel.RectOf(quality.node);
    const float middle = Middle(track).y;
    panel.Step(Pressing({.x = track.x + (track.width / 2.f), .y = middle}));
    REQUIRE(panel.events.Read<StepPicked>().size() == 1);
    CHECK(panel.events.Read<StepPicked>()[0].step == 2);

    // Dragging across a step's width moves it one step, and no further.
    panel.Step(Holding({.x = track.x + (track.width * 0.55f), .y = middle}));
    CHECK(panel.events.Read<StepPicked>().size() == 1); // still the same step

    panel.Step(Holding({.x = track.x + track.width, .y = middle}));
    REQUIRE(panel.events.Read<StepPicked>().size() == 2);
    CHECK(panel.events.Read<StepPicked>()[1].step == kSteps - 1);
}

TEST_CASE("Widgets: a stepped slider moves one step per key press and stops at its ends")
{
    Panel panel;
    constexpr int32_t kSteps = 3;
    const SteppedSliderId quality = panel.screen->AddSteppedSlider(panel.row, kQuality, kSteps, 1);
    panel.screen->OnChange(quality, [](int32_t step) { return StepPicked{step}; });
    panel.Step({});
    panel.ui.SetFocus(*panel.screen, quality.node);

    panel.Step(Pressing(UiAction::Right));
    REQUIRE(panel.events.Read<StepPicked>().size() == 1);
    CHECK(panel.events.Read<StepPicked>()[0].step == 2);

    panel.Step(Pressing(UiAction::Right)); // already at the last step
    CHECK(panel.events.Read<StepPicked>().size() == 1);
    CHECK(panel.ui.GetInteraction().focused == quality.node);

    panel.Step(Pressing(UiAction::Left));
    REQUIRE(panel.events.Read<StepPicked>().size() == 2);
    CHECK(panel.events.Read<StepPicked>()[1].step == 1);
}

TEST_CASE("Widgets: the game sets a stepped slider's step, within the steps it has")
{
    Panel panel;
    const SteppedSliderId quality = panel.screen->AddSteppedSlider(panel.row, kQuality, 4, 0);
    panel.screen->OnChange(quality, [](int32_t step) { return StepPicked{step}; });
    panel.Step({});

    panel.screen->SetValue(quality, 9); // past the end, so it rests on the last step
    panel.Step({});
    CHECK(panel.events.Read<StepPicked>().empty()); // the game's own set announces nothing

    panel.ui.SetFocus(*panel.screen, quality.node);
    panel.Step(Pressing(UiAction::Left));
    REQUIRE(panel.events.Read<StepPicked>().size() == 1);
    CHECK(panel.events.Read<StepPicked>()[0].step == 2);
}

TEST_CASE("Widgets: the game sets a slider's value, except while the player is dragging it")
{
    Panel panel;
    const ContinuousSliderId volume = panel.screen->AddContinuousSlider(panel.row, kPercent, 0.f);
    panel.Step({});

    panel.screen->SetValue(volume, 25.f);
    panel.Step({});
    CHECK(panel.screen->GetValue(volume) == doctest::Approx(25.f));
    const Rect track = panel.RectOf(volume.node);

    // Just inside its right edge, which the slider owns; the edge itself is past it.
    panel.Step(Pressing({.x = track.x + track.width - 1.f, .y = Middle(track).y}));
    panel.screen->SetValue(volume, 0.f); // the player owns it mid-drag
    CHECK(panel.screen->GetValue(volume) == doctest::Approx(kPercent.max));
    panel.screen->OnChange(volume, [](float value) { return VolumeChanged{value}; });
    panel.Step(Holding({.x = track.x + track.width, .y = Middle(track).y}));
    panel.Step(Releasing({.x = track.x + track.width, .y = Middle(track).y}));

    panel.screen->SetValue(volume, 0.f);
    panel.Step({});
    REQUIRE(panel.events.Read<VolumeChanged>().empty()); // the game's own set announces nothing
}

TEST_CASE("Widgets: a slider's buttons step it, and the track keeps clear of them")
{
    Panel panel;
    const ContinuousSliderId volume = panel.screen->AddContinuousSlider(panel.row, kPercent, 50.f);
    panel.screen->SetButtons(volume, SliderButtons::Shown);
    panel.screen->OnChange(volume, [](float value) { return VolumeChanged{value}; });
    panel.Step({});

    const Rect rect = panel.RectOf(volume.node);
    const float middle = Middle(rect).y;
    panel.Step(Pressing({.x = rect.x + 2.f, .y = middle}));
    REQUIRE(panel.events.Read<VolumeChanged>().size() == 1);
    CHECK(panel.events.Read<VolumeChanged>()[0].volume == doctest::Approx(50.f - kPercent.step));
    panel.Step(Releasing({.x = rect.x + 2.f, .y = middle}));

    panel.Step(Pressing({.x = rect.x + rect.width - 2.f, .y = middle}));
    REQUIRE(panel.events.Read<VolumeChanged>().size() == 2);
    CHECK(panel.events.Read<VolumeChanged>()[1].volume == doctest::Approx(50.f));
    panel.Step(Releasing({.x = rect.x + rect.width - 2.f, .y = middle}));

    // Dragging off a button does not throw the handle to the pointer.
    panel.Step(Pressing({.x = rect.x + 2.f, .y = middle}));
    panel.Step(Holding({.x = Middle(rect).x, .y = middle}));
    CHECK(panel.screen->GetValue(volume) == doctest::Approx(50.f - kPercent.step));

    // The handle sits inside the track, which starts past the first button.
    CHECK(panel.screen->GetThumbRect(volume).x > rect.x);
}

TEST_CASE("Widgets: a slider says what it reads, where its handle is, and what each step stands for")
{
    Panel panel;
    const ContinuousSliderId volume = panel.screen->AddContinuousSlider(panel.row, kPercent, 25.f);
    constexpr int32_t kSteps = 4;
    const SteppedSliderId quality = panel.screen->AddSteppedSlider(panel.row, kQuality, kSteps, 1);
    panel.Step({});

    CHECK(panel.screen->GetValue(volume) == doctest::Approx(25.f));
    CHECK(panel.screen->GetFraction(volume) == doctest::Approx(0.25f));
    CHECK(panel.screen->GetRange(volume).max == doctest::Approx(kPercent.max));

    CHECK(panel.screen->GetValue(quality) == 1);
    CHECK(panel.screen->GetSteps(quality) == kSteps);
    CHECK(panel.screen->GetFraction(quality) == doctest::Approx(1.f / 3.f));
    CHECK(panel.screen->GetStepValue(quality, 0) == doctest::Approx(kQuality.min));
    CHECK(panel.screen->GetStepValue(quality, kSteps - 1) == doctest::Approx(kQuality.max));
    CHECK(panel.screen->GetStepValue(quality, 2) == doctest::Approx(2.f));

    // The handle is where the value says, and inside the slider.
    const Rect thumb = panel.screen->GetThumbRect(volume);
    const Rect rect = panel.RectOf(volume.node);
    CHECK(thumb.width > 0.f);
    CHECK(thumb.x >= rect.x);
    CHECK(thumb.x + thumb.width <= rect.x + rect.width);
    CHECK(thumb.x > rect.x); // a quarter of the way along, not at the start
}

TEST_CASE("Widgets: a toggle flips on a click and on Accept, and says which way it went")
{
    Panel panel;
    const ToggleId mute = panel.screen->AddToggle(panel.row, false);
    panel.screen->OnChange(mute, [](bool on) { return SwitchFlipped{on}; });
    panel.Step({});

    const Point centre = Middle(panel.RectOf(mute.node));
    panel.Step(Pressing(centre));
    panel.Step(Releasing(centre));
    REQUIRE(panel.events.Read<SwitchFlipped>().size() == 1);
    CHECK(panel.events.Read<SwitchFlipped>()[0].on);

    panel.ui.SetFocus(*panel.screen, mute.node);
    panel.Step(Pressing(UiAction::Accept));
    REQUIRE(panel.events.Read<SwitchFlipped>().size() == 2);
    CHECK_FALSE(panel.events.Read<SwitchFlipped>()[1].on);
}

TEST_CASE("Widgets: a scroll container takes the wheel over anything inside it, and stops at the end")
{
    Panel panel;
    Style tall;
    tall.sizing = {Sizing::Fixed(200.f), Sizing::Fixed(100.f)};
    tall.direction = Direction::Column;
    const NodeId list = panel.screen->AddScroll(panel.row, tall, {false, true});
    Style entry;
    entry.sizing = {Sizing::Fixed(200.f), Sizing::Fixed(80.f)};
    for (const std::string_view label : {"One", "Two", "Three"})
    {
        panel.screen->Tree().SetStyle(panel.screen->AddButton(list, label).node, entry);
    }
    panel.Step({});

    const Point inside = Middle(panel.RectOf(list));
    const InputResult used = panel.Step(Wheeling(inside, -1.f));
    CHECK(used.wheelUsed);
    const float first = panel.screen->Tree().Get(list)->scrollOffset.y;
    CHECK(first > 0.f);

    // However much is asked for, it stops where its content does.
    for (uint32_t turn = 0; turn < 20; ++turn)
    {
        panel.Step(Wheeling(inside, -1.f));
    }
    const float furthest = panel.screen->Tree().Get(list)->scrollOffset.y;
    CHECK(furthest > first);
    panel.Step(Wheeling(inside, -1.f));
    CHECK(panel.screen->Tree().Get(list)->scrollOffset.y == doctest::Approx(furthest));

    // And back up to where it started.
    for (uint32_t turn = 0; turn < 30; ++turn)
    {
        panel.Step(Wheeling(inside, 1.f));
    }
    CHECK(panel.screen->Tree().Get(list)->scrollOffset.y == doctest::Approx(0.f));
}

TEST_CASE("Widgets: a scroll container the game makes focusable scrolls on the vertical keys")
{
    Panel panel;
    Style tall;
    tall.sizing = {Sizing::Fixed(200.f), Sizing::Fixed(100.f)};
    tall.direction = Direction::Column;
    const NodeId log = panel.screen->AddScroll(panel.row, tall, {false, true});
    Style line;
    line.sizing = {Sizing::Fixed(200.f), Sizing::Fixed(80.f)};
    for (uint32_t index = 0; index < 3; ++index)
    {
        panel.screen->Tree().SetStyle(panel.screen->Tree().Create(log), line);
    }
    // Out of the Tab order until a game says otherwise: a container holding its
    // own controls is reached through them.
    CHECK_FALSE(panel.screen->Tree().Get(log)->focusable);

    panel.screen->Tree().SetFocusable(log, true);
    panel.Step({});
    panel.ui.SetFocus(*panel.screen, log);

    panel.Step(Pressing(UiAction::Down));
    const float scrolled = panel.screen->Tree().Get(log)->scrollOffset.y;
    CHECK(scrolled > 0.f);
    CHECK(panel.ui.GetInteraction().focused == log); // the keys scrolled it rather than leaving it

    panel.Step(Pressing(UiAction::Up));
    CHECK(panel.screen->Tree().Get(log)->scrollOffset.y < scrolled);
}

namespace
{

/// A scrolling list of @p rows rows, each as tall as the list itself is short.
NodeId ListOf(Panel &panel, uint32_t rows, Style style)
{
    style.sizing = {Sizing::Fixed(200.f), Sizing::Fixed(100.f)};
    style.direction = Direction::Column;
    const NodeId list = panel.screen->AddScroll(panel.row, style, {false, true});
    Style line;
    line.sizing = {Sizing::Fixed(200.f), Sizing::Fixed(80.f)};
    for (uint32_t index = 0; index < rows; ++index)
    {
        panel.screen->Tree().SetStyle(panel.screen->Tree().Create(list), line);
    }
    panel.Step({});
    return list;
}

/// The last two quads drawn, which are a scroll bar's track and thumb.
Rect ThumbOf(const Panel &panel)
{
    const auto quads = panel.ui.GetDrawList().Instances();
    REQUIRE(quads.size() >= 2);
    return quads.back().rect;
}

} // namespace

TEST_CASE("Widgets: a scroll bar shows only while it is needed, or always, or never")
{
    Panel panel;
    Style style;
    const NodeId shortList = ListOf(panel, 1, style); // its one row fits
    const std::size_t bare = panel.ui.GetDrawList().Instances().size();

    style.enabledScrollBars = {false, true}; // AddScroll set these; a whole style replaces them
    style.sizing = {Sizing::Fixed(200.f), Sizing::Fixed(100.f)};
    style.direction = Direction::Column;
    style.scrollBarVisibility = ScrollBarVisibility::Always;
    panel.screen->Tree().SetStyle(shortList, style);
    panel.Step({});
    CHECK(panel.ui.GetDrawList().Instances().size() == bare + 2); // a track and a thumb

    style.scrollBarVisibility = ScrollBarVisibility::WhenNeeded;
    panel.screen->Tree().SetStyle(shortList, style);
    panel.Step({});
    CHECK(panel.ui.GetDrawList().Instances().size() == bare); // nothing is out of sight

    style.scrollBarVisibility = ScrollBarVisibility::Never;
    panel.screen->Tree().SetStyle(shortList, style);
    panel.Step({});
    CHECK(panel.ui.GetDrawList().Instances().size() == bare);
}

TEST_CASE("Widgets: a scroll bar's thumb never shrinks past the size the style asks for")
{
    Panel panel;
    Style style;
    style.scrollBarVisibility = ScrollBarVisibility::WhenNeeded;
    constexpr float kMinimum = 30.f;
    style.scrollBarMinLength = kMinimum;
    ListOf(panel, 40, style); // far more content than the list is tall

    CHECK(ThumbOf(panel).height == doctest::Approx(kMinimum));
}

TEST_CASE("Widgets: dragging a scroll bar's thumb scrolls the content it stands for")
{
    Panel panel;
    Style style;
    style.scrollBarVisibility = ScrollBarVisibility::WhenNeeded;
    const NodeId list = ListOf(panel, 3, style);
    const Rect thumb = ThumbOf(panel);

    const Point grab{.x = thumb.x + (thumb.width / 2.f), .y = thumb.y + (thumb.height / 2.f)};
    panel.Step(Pressing(grab));
    constexpr float kDragged = 20.f;
    panel.Step(Holding({.x = grab.x, .y = grab.y + kDragged}));

    // The content moves further than the thumb, by as much more as it is longer.
    const float scrolled = panel.screen->Tree().Get(list)->scrollTarget.y;
    CHECK(scrolled > kDragged);

    panel.Step(Releasing({.x = grab.x, .y = grab.y + kDragged}));
    panel.Step(Holding({.x = grab.x, .y = grab.y + (2.f * kDragged)}));
    CHECK(panel.screen->Tree().Get(list)->scrollTarget.y == doctest::Approx(scrolled)); // let go, so it stays
}

TEST_CASE("Widgets: a scroll container may glide to where it is going instead of jumping")
{
    Panel panel;
    Style tall;
    tall.sizing = {Sizing::Fixed(200.f), Sizing::Fixed(100.f)};
    tall.direction = Direction::Column;
    constexpr float kSmoothing = 0.2f;
    tall.scrollSmoothing = kSmoothing;
    const NodeId list = panel.screen->AddScroll(panel.row, tall, {false, true});
    Style line;
    line.sizing = {Sizing::Fixed(200.f), Sizing::Fixed(80.f)};
    for (uint32_t index = 0; index < 3; ++index)
    {
        panel.screen->Tree().SetStyle(panel.screen->Tree().Create(list), line);
    }
    panel.Step({});

    const Point inside = Middle(panel.RectOf(list));
    UiInput wheel = Wheeling(inside, -1.f);
    wheel.time = 0.0;
    panel.Step(wheel);
    const float target = panel.screen->Tree().Get(list)->scrollTarget.y;
    CHECK(target > 0.f);
    CHECK(panel.screen->Tree().Get(list)->scrollOffset.y < target); // it has not arrived yet

    UiInput settling = At(inside);
    settling.time = static_cast<double>(kSmoothing) / 2.0;
    panel.Step(settling);
    const float part = panel.screen->Tree().Get(list)->scrollOffset.y;
    CHECK(part > 0.f);
    CHECK(part < target);

    settling.time = 1.0;
    panel.Step(settling);
    CHECK(panel.screen->Tree().Get(list)->scrollOffset.y == doctest::Approx(target));
}

TEST_CASE("Widgets: without smoothing a scroll container is where it is going at once")
{
    Panel panel;
    Style tall;
    tall.sizing = {Sizing::Fixed(200.f), Sizing::Fixed(100.f)};
    tall.direction = Direction::Column;
    const NodeId list = panel.screen->AddScroll(panel.row, tall, {false, true});
    Style line;
    line.sizing = {Sizing::Fixed(200.f), Sizing::Fixed(80.f)};
    for (uint32_t index = 0; index < 3; ++index)
    {
        panel.screen->Tree().SetStyle(panel.screen->Tree().Create(list), line);
    }
    panel.Step({});

    panel.Step(Wheeling(Middle(panel.RectOf(list)), -1.f));
    const Node *node = panel.screen->Tree().Get(list);
    CHECK(node->scrollOffset.y == doctest::Approx(node->scrollTarget.y));
    CHECK(node->scrollOffset.y > 0.f);
}

TEST_CASE("Widgets: a scroll bar takes a press that lands on it, over whatever it covers")
{
    Panel panel;
    Style style;
    style.scrollBarVisibility = ScrollBarVisibility::WhenNeeded;
    style.sizing = {Sizing::Fixed(200.f), Sizing::Fixed(100.f)};
    style.direction = Direction::Column;
    const NodeId list = panel.screen->AddScroll(panel.row, style, {false, true});

    // Rows that fill the width, so they lie under the bar as a real list's do.
    Style row;
    row.sizing = {Sizing::Grow(), Sizing::Fixed(80.f)};
    for (const std::string_view label : {"One", "Two", "Three"})
    {
        panel.screen->Tree().SetStyle(panel.screen->AddButton(list, label).node, row);
    }
    panel.Step({});

    const Rect thumb = ThumbOf(panel);
    const Point grab{.x = thumb.x + (thumb.width / 2.f), .y = thumb.y + (thumb.height / 2.f)};
    panel.Step(Pressing(grab));
    CHECK(panel.ui.GetInteraction().pressed == list); // the bar, not the row beneath it

    panel.Step(Holding({.x = grab.x, .y = grab.y + 20.f}));
    CHECK(panel.screen->Tree().Get(list)->scrollTarget.y > 0.f);
}

TEST_CASE("Widgets: a dragged bar follows the pointer, and glides only where the style asks it to")
{
    for (const ScrollBarDrag drag : {ScrollBarDrag::FollowsPointer, ScrollBarDrag::Smoothed})
    {
        const bool smoothed = drag == ScrollBarDrag::Smoothed;
        CAPTURE(smoothed);
        Panel panel;
        Style style;
        style.scrollBarVisibility = ScrollBarVisibility::WhenNeeded;
        style.scrollSmoothing = 0.2f;
        style.scrollBarDrag = drag;
        const NodeId list = ListOf(panel, 3, style);

        const Rect thumb = ThumbOf(panel);
        const Point grab{.x = thumb.x + (thumb.width / 2.f), .y = thumb.y + (thumb.height / 2.f)};
        panel.Step(Pressing(grab));
        panel.Step(Holding({.x = grab.x, .y = grab.y + 20.f}));

        const Node *node = panel.screen->Tree().Get(list);
        CHECK(node->scrollTarget.y > 0.f);
        if (smoothed)
        {
            CHECK(node->scrollOffset.y < node->scrollTarget.y);
        }
        else
        {
            CHECK(node->scrollOffset.y == doctest::Approx(node->scrollTarget.y));
        }
    }
}

TEST_CASE("Widgets: a container that scrolls only sideways takes a plain wheel sideways")
{
    Panel panel;
    Style wide;
    wide.sizing = {Sizing::Fixed(100.f), Sizing::Fixed(100.f)};
    const NodeId strip = panel.screen->AddScroll(panel.row, wide, {true, false});
    Style card;
    card.sizing = {Sizing::Fixed(80.f), Sizing::Fixed(80.f)};
    for (uint32_t index = 0; index < 4; ++index)
    {
        panel.screen->Tree().SetStyle(panel.screen->Tree().Create(strip), card);
    }
    panel.Step({});

    CHECK(panel.Step(Wheeling(Middle(panel.RectOf(strip)), -1.f)).wheelUsed);
    CHECK(panel.screen->Tree().Get(strip)->scrollTarget.x > 0.f);
    CHECK(panel.screen->Tree().Get(strip)->scrollTarget.y == doctest::Approx(0.f));
}

TEST_CASE("Widgets: a container that scrolls both ways takes the wheel down and Shift's wheel sideways")
{
    Panel panel;
    Style both;
    both.sizing = {Sizing::Fixed(100.f), Sizing::Fixed(100.f)};
    both.direction = Direction::Column;
    both.scrollBarVisibility = ScrollBarVisibility::WhenNeeded;
    const NodeId pane = panel.screen->AddScroll(panel.row, both, {true, true});
    Style block;
    block.sizing = {Sizing::Fixed(300.f), Sizing::Fixed(80.f)};
    for (uint32_t index = 0; index < 3; ++index)
    {
        panel.screen->Tree().SetStyle(panel.screen->Tree().Create(pane), block);
    }
    panel.Step({});
    const std::size_t withBars = panel.ui.GetDrawList().Instances().size();

    const Point inside = Middle(panel.RectOf(pane));
    panel.Step(Wheeling(inside, -1.f));
    CHECK(panel.screen->Tree().Get(pane)->scrollTarget.y > 0.f);
    CHECK(panel.screen->Tree().Get(pane)->scrollTarget.x == doctest::Approx(0.f));

    // The host turns Shift and the wheel into a sideways turn.
    UiInput sideways = At(inside);
    sideways.wheel = {.x = -1.f, .y = 0.f};
    panel.Step(sideways);
    CHECK(panel.screen->Tree().Get(pane)->scrollTarget.x > 0.f);

    // One bar per axis, over what the pane itself drew.
    Style bare = both;
    bare.scrollBarVisibility = ScrollBarVisibility::Never;
    bare.enabledScrollBars = {true, true};
    panel.screen->Tree().SetStyle(pane, bare);
    panel.Step({});
    CHECK(panel.ui.GetDrawList().Instances().size() == withBars - 4);
}

TEST_CASE("Widgets: dragging a scroll container's background scrolls it")
{
    Panel panel;
    Style tall;
    tall.sizing = {Sizing::Fixed(200.f), Sizing::Fixed(100.f)};
    tall.direction = Direction::Column;
    const NodeId list = panel.screen->AddScroll(panel.row, tall, {false, true});
    Style row;
    row.sizing = {Sizing::Fixed(200.f), Sizing::Fixed(80.f)};
    for (uint32_t index = 0; index < 3; ++index)
    {
        panel.screen->Tree().SetStyle(panel.screen->Tree().Create(list), row);
    }
    panel.Step({});

    const Rect rect = panel.RectOf(list);
    const Point start{.x = rect.x + rect.width - 2.f, .y = rect.y + rect.height - 2.f};
    panel.Step(Pressing(start));
    constexpr float kDragged = 30.f;
    panel.Step(Holding({.x = start.x, .y = start.y - kDragged}));
    CHECK(panel.screen->Tree().Get(list)->scrollOffset.y == doctest::Approx(kDragged));
}
