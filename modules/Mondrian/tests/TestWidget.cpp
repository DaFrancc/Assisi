/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Mondrian/Widget.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <vector>

using namespace Assisi::Mondrian;

namespace
{

constexpr Extent kScreen{1920, 1080};
constexpr float kProbeWidth = 300.f;
constexpr float kProbeHeight = 40.f;

/// What the probe widget below was asked to do, in order.
struct Record
{
    std::vector<WidgetGesture> gestures;
    std::vector<UiAction> actions;
    Point lastPointer;
    Point lastDelta;
    Point lastWheel;
    uint32_t measured = 0;
    uint32_t drawn = 0;
    WidgetResponse answer = WidgetResponse::Handled;
};

Point ProbeMeasure(const Node & /*node*/, void *context)
{
    ++static_cast<Record *>(context)->measured;
    return {.x = kProbeWidth, .y = kProbeHeight};
}

void ProbeDraw(const WidgetView &view, DrawList &list)
{
    ++static_cast<Record *>(view.context)->drawn;
    list.Quad(view.layout->rect).Fill({1.f, 0.f, 1.f, 1.f});
}

WidgetResponse ProbeInput(const WidgetView &view, Node & /*node*/, const WidgetEvent &event)
{
    Record &record = *static_cast<Record *>(view.context);
    record.gestures.push_back(event.gesture);
    record.lastPointer = event.pointer;
    record.lastDelta = event.pointerDelta;
    record.lastWheel = event.wheel;
    if (event.gesture == WidgetGesture::Action)
    {
        record.actions.push_back(event.action);
    }
    return record.answer;
}

/// A UI holding one probe widget, laid out once so input has something to hit.
struct Probe
{
    Record record;
    Ui ui;
    Screen *screen = nullptr;
    NodeId node;

    Probe()
    {
        screen = ui.CreateScreen(ScreenKind::Stacked, kSortMenu, "probe-screen");
        ui.Show(*screen);

        const uint32_t type = screen->Tree().Widgets().Register(
            WidgetType{.measure = &ProbeMeasure, .draw = &ProbeDraw, .input = &ProbeInput, .context = &record});
        Style style;
        style.floating.enabled = true;
        node = screen->Tree().Create(screen->Root(), "probe");
        screen->Tree().SetStyle(node, style);
        screen->Tree().SetBehaviour(node, type);
        screen->Tree().SetFocusable(node, true);
        Step({});
    }

    InputResult Step(const UiInput &input)
    {
        const InputResult result = ui.ProcessInput(input);
        ui.Sync(kScreen);
        return result;
    }

    /// The probe's centre, where the pointer lands on it.
    [[nodiscard]] Point Centre() const
    {
        const LayoutNode *placed = screen->GetLayout().Get(screen->Find("probe"));
        REQUIRE(placed != nullptr);
        return {.x = placed->rect.x + (placed->rect.width / 2.f), .y = placed->rect.y + (placed->rect.height / 2.f)};
    }
};

UiInput At(Point pointer, InputGrant grant = InputGrant::Everything)
{
    UiInput input;
    input.pointer = pointer;
    input.grant = grant;
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

UiInput Pressing(UiAction action)
{
    UiInput input;
    input.grant = InputGrant::Everything;
    input.actionPressed[static_cast<std::size_t>(action)] = true;
    input.actionDown[static_cast<std::size_t>(action)] = true;
    return input;
}

bool Has(const std::vector<WidgetGesture> &gestures, WidgetGesture gesture)
{
    return std::ranges::find(gestures, gesture) != gestures.end();
}

} // namespace

TEST_CASE("Widget: a type's measure decides the size of the node it is on")
{
    Probe probe;
    const LayoutNode *node = probe.screen->GetLayout().Get(probe.node);
    REQUIRE(node != nullptr);
    CHECK(node->rect.width == doctest::Approx(kProbeWidth));
    CHECK(node->rect.height == doctest::Approx(kProbeHeight));
    CHECK(probe.record.measured > 0);
}

TEST_CASE("Widget: a type's draw adds to the draw list over the node's own quads")
{
    Probe probe;
    CHECK(probe.record.drawn > 0);

    const std::size_t withWidget = probe.ui.GetDrawList().Instances().size();
    probe.screen->Tree().SetBehaviour(probe.node, 0);
    probe.Step({});
    CHECK(probe.ui.GetDrawList().Instances().size() == withWidget - 1);
}

TEST_CASE("Widget: a click reaches the type as a press, then a release, then an activation")
{
    Probe probe;
    const Point centre = probe.Centre();
    probe.Step(Pressing(centre));
    CHECK(probe.record.gestures == std::vector{WidgetGesture::Press});
    CHECK(probe.record.lastPointer.x == doctest::Approx(centre.x));

    probe.record.gestures.clear();
    probe.Step(Releasing(centre));
    CHECK(probe.record.gestures == std::vector{WidgetGesture::Release, WidgetGesture::Activate});
}

TEST_CASE("Widget: the pointer moving while held reaches the type as a drag, with how far it moved")
{
    Probe probe;
    const Point centre = probe.Centre();
    probe.Step(Pressing(centre));
    probe.record.gestures.clear();

    constexpr float kMoved = 12.f;
    probe.Step(Holding({.x = centre.x + kMoved, .y = centre.y}));
    CHECK(probe.record.gestures == std::vector{WidgetGesture::Drag});
    CHECK(probe.record.lastDelta.x == doctest::Approx(kMoved));

    // The press is kept while the pointer leaves, so a drag still arrives.
    probe.record.gestures.clear();
    probe.Step(Holding({.x = centre.x + (2.f * kMoved), .y = centre.y + kProbeHeight}));
    CHECK(probe.record.gestures == std::vector{WidgetGesture::Drag});
}

TEST_CASE("Widget: accepting a focused widget reaches it as an action before it becomes an activation")
{
    Probe probe;
    probe.ui.SetFocus(*probe.screen, probe.node);

    // A control that takes Accept keeps it: a field puts a newline in rather
    // than reading Enter as the click a button would.
    probe.record.answer = WidgetResponse::Handled;
    probe.Step(Pressing(UiAction::Accept));
    CHECK(probe.record.gestures == std::vector{WidgetGesture::Action});
    CHECK(probe.record.actions == std::vector{UiAction::Accept});

    // One that passes on Accept is activated by it, as a button is.
    probe.record.gestures.clear();
    probe.record.answer = WidgetResponse::Ignored;
    probe.Step(Pressing(UiAction::Accept));
    CHECK(probe.record.gestures == std::vector{WidgetGesture::Action, WidgetGesture::Activate});
}

TEST_CASE("Widget: a focused widget may take Back, and the screen closes only when none does")
{
    Probe probe;
    probe.ui.SetFocus(*probe.screen, probe.node);

    probe.record.answer = WidgetResponse::Handled;
    probe.Step(Pressing(UiAction::Back));
    CHECK(probe.record.actions == std::vector{UiAction::Back});
    CHECK(probe.screen->IsShown());

    probe.record.answer = WidgetResponse::Ignored;
    probe.Step(Pressing(UiAction::Back));
    CHECK_FALSE(probe.screen->IsShown());
}

TEST_CASE("Widget: the wheel reaches the widget under the pointer")
{
    Probe probe;
    UiInput input = At(probe.Centre());
    constexpr float kNotches = -2.f;
    input.wheel = {.x = 0.f, .y = kNotches};
    const InputResult result = probe.Step(input);

    CHECK(Has(probe.record.gestures, WidgetGesture::Wheel));
    CHECK(probe.record.lastWheel.y == doctest::Approx(kNotches));
    CHECK(result.wheelUsed);
}

TEST_CASE("Widget: the wheel over nothing is the game's")
{
    Probe probe;
    UiInput input = At({.x = 1900.f, .y = 1000.f});
    input.wheel = {.x = 0.f, .y = 1.f};
    CHECK_FALSE(probe.Step(input).wheelUsed);
    CHECK_FALSE(Has(probe.record.gestures, WidgetGesture::Wheel));
}

TEST_CASE("Widget: a direction the widget handles does not also move focus")
{
    Probe probe;
    Style style;
    style.floating.enabled = true;
    style.floating.offset = {.x = 600.f, .y = 0.f};
    style.sizing = {Sizing::Fixed(100.f), Sizing::Fixed(40.f)};
    const NodeId neighbour = probe.screen->Tree().Create(probe.screen->Tree().Root(), "neighbour");
    probe.screen->Tree().SetStyle(neighbour, style);
    probe.screen->Tree().SetFocusable(neighbour, true);
    probe.Step({});

    probe.ui.SetFocus(*probe.screen, probe.node);
    probe.record.answer = WidgetResponse::Handled;
    probe.Step(Pressing(UiAction::Right));
    CHECK(probe.record.actions == std::vector{UiAction::Right});
    CHECK(probe.ui.GetInteraction().focused == probe.node);

    probe.record.answer = WidgetResponse::Ignored;
    probe.Step(Pressing(UiAction::Right));
    CHECK(probe.ui.GetInteraction().focused == neighbour);
}
