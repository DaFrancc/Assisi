/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Layout.hpp>
#include <Assisi/Mondrian/NodeTree.hpp>
#include <Assisi/Mondrian/Widget.hpp>

#include <algorithm>
#include <cmath>
#include <variant>

namespace Assisi::Mondrian
{
namespace
{

/// The built-ins' own look, until themes decide it.
constexpr Math::Color4<Math::ColorSpace::Srgb> kTrackColor{0.24f, 0.26f, 0.32f, 1.f};
constexpr Math::Color4<Math::ColorSpace::Srgb> kFillColor{0.90f, 0.20f, 0.10f, 1.f};
constexpr Math::Color4<Math::ColorSpace::Srgb> kKnobColor{0.92f, 0.93f, 0.96f, 1.f};

/// Sizes in UI pixels.
constexpr float kToggleWidth = 56.f;
constexpr float kToggleHeight = 28.f;
constexpr float kSliderWidth = 220.f;
constexpr float kSliderHeight = 28.f;
constexpr float kSliderTrackHeight = 6.f;
constexpr float kSliderThumbWidth = 16.f;

/// A slider's end buttons: how far they sit from its track, how round they are,
/// and the marks across them, all in UI pixels.
constexpr float kSliderButtonGap = 6.f;
constexpr float kSliderButtonRadius = 6.f;
constexpr float kSliderMarkLength = 12.f;
constexpr float kSliderMarkThickness = 2.f;

/// How far one notch of the wheel scrolls, in UI pixels, and how far one
/// press of a key does.
constexpr float kWheelStep = 48.f;
constexpr float kScrollKeyStep = 40.f;

/// How wide the bar showing where a scrolling node is sits, in UI pixels.
constexpr float kScrollBarThickness = 6.f;

/// @p value as a fraction, or zero when the node holds something else.
float Fraction(const WidgetValue &value)
{
    const float *held = std::get_if<float>(&value);
    return held != nullptr ? *held : 0.f;
}

bool Switched(const WidgetValue &value)
{
    const bool *held = std::get_if<bool>(&value);
    return held != nullptr && *held;
}

/// Whether @p point is on @p rect, which a bar or a button is pressed by.
bool Covers(const Rect &rect, Point point)
{
    return point.x >= rect.x && point.x <= rect.x + rect.width && point.y >= rect.y && point.y <= rect.y + rect.height;
}

/// The part of @p rect a slider's thumb slides along, in device pixels.
float ThumbTravel(const Rect &rect, float scale)
{
    return std::max(0.f, rect.width - (kSliderThumbWidth * scale));
}

Point MeasureToggle(const Node & /*node*/, void * /*context*/)
{
    return {.x = kToggleWidth, .y = kToggleHeight};
}

void DrawToggle(const WidgetView &view, DrawList &list)
{
    const Rect &rect = view.layout->rect;
    const bool on = Switched(view.node->value);
    const float knob = rect.height;
    list.Quad(rect).Fill(on ? kFillColor : kTrackColor).Corners(rect.height / 2.f, CornerStyle::Rounded);
    list.Quad(Rect{.x = on ? rect.x + rect.width - knob : rect.x, .y = rect.y, .width = knob, .height = knob})
        .Fill(kKnobColor)
        .Corners(knob / 2.f, CornerStyle::Rounded);
}

WidgetResponse ToggleInput(const WidgetView & /*view*/, Node &node, const WidgetEvent &event)
{
    if (event.gesture != WidgetGesture::Activate)
    {
        // A press is still the toggle's: it must not fall through to the game.
        return event.gesture == WidgetGesture::Press ? WidgetResponse::Handled : WidgetResponse::Ignored;
    }
    node.value = !Switched(node.value);
    return WidgetResponse::Changed;
}

Point MeasureSlider(const Node & /*node*/, void * /*context*/)
{
    return {.x = kSliderWidth, .y = kSliderHeight};
}

/// Which step @p node rests on, or zero when it holds something else.
int32_t Step(const WidgetValue &value)
{
    const int32_t *held = std::get_if<int32_t>(&value);
    return held != nullptr ? *held : 0;
}

/// How far along its range a continuous slider's value sits, from 0 to 1, which
/// is where its thumb goes.
float ContinuousFraction(const Node &node)
{
    const float span = node.range.max - node.range.min;
    return span > 0.f ? std::clamp((Fraction(node.value) - node.range.min) / span, 0.f, 1.f) : 0.f;
}

/// How far along a stepped slider its step sits, from 0 to 1.
float StepFraction(const Node &node)
{
    return node.steps > 1 ? static_cast<float>(Step(node.value)) / static_cast<float>(node.steps - 1) : 0.f;
}

/// Where a slider's parts sit: its track, and the buttons at either end when it
/// carries them.
struct SliderParts
{
    Rect decrement;
    Rect increment;
    Rect track;
    bool buttoned = false;
};

SliderParts PlaceSlider(const WidgetView &view)
{
    const Rect &rect = view.layout->rect;
    SliderParts parts;
    parts.buttoned = view.node->sliderButtons == SliderButtons::Shown;
    if (!parts.buttoned)
    {
        parts.track = rect;
        return parts;
    }
    // Square ends, so a button is as tall as the slider and no wider.
    const float side = std::min(rect.height, rect.width / 2.f);
    parts.decrement = Rect{.x = rect.x, .y = rect.y, .width = side, .height = rect.height};
    parts.increment = Rect{.x = rect.x + rect.width - side, .y = rect.y, .width = side, .height = rect.height};
    parts.track = Rect{.x = rect.x + side + (kSliderButtonGap * view.scale),
                       .y = rect.y,
                       .width = std::max(0.f, rect.width - (2.f * (side + (kSliderButtonGap * view.scale)))),
                       .height = rect.height};
    return parts;
}

/// Draws one end button, with a bar across it and, when it adds, one down it.
void DrawSliderButton(DrawList &list, const Rect &rect, float scale, bool adds)
{
    const float mark = kSliderMarkLength * scale;
    const float thickness = kSliderMarkThickness * scale;
    const Point centre{.x = rect.x + (rect.width / 2.f), .y = rect.y + (rect.height / 2.f)};

    list.Quad(rect).Fill(kTrackColor).Corners(kSliderButtonRadius * scale, CornerStyle::Rounded);
    list.Quad(Rect{.x = std::round(centre.x - (mark / 2.f)),
                   .y = std::round(centre.y - (thickness / 2.f)),
                   .width = mark,
                   .height = thickness})
        .Fill(kKnobColor);
    if (adds)
    {
        list.Quad(Rect{.x = std::round(centre.x - (thickness / 2.f)),
                       .y = std::round(centre.y - (mark / 2.f)),
                       .width = thickness,
                       .height = mark})
            .Fill(kKnobColor);
    }
}

/// Draws a slider's track, the part of it behind the thumb, the thumb @p
/// fraction of the way along, and its buttons when it has them.
void DrawSliderAt(const WidgetView &view, DrawList &list, float fraction)
{
    const SliderParts parts = PlaceSlider(view);
    const Rect &track = parts.track;
    const float thumb = kSliderThumbWidth * view.scale;
    const float trackHeight = kSliderTrackHeight * view.scale;
    const float trackY = std::round(track.y + ((track.height - trackHeight) / 2.f));
    const float travel = ThumbTravel(track, view.scale);

    list.Quad(Rect{.x = track.x, .y = trackY, .width = track.width, .height = trackHeight})
        .Fill(kTrackColor)
        .Corners(trackHeight / 2.f, CornerStyle::Rounded);
    list
        .Quad(Rect{
            .x = track.x, .y = trackY, .width = std::round((travel * fraction) + (thumb / 2.f)), .height = trackHeight})
        .Fill(kFillColor)
        .Corners(trackHeight / 2.f, CornerStyle::Rounded);
    list.Quad(
            Rect{.x = std::round(track.x + (travel * fraction)), .y = track.y, .width = thumb, .height = track.height})
        .Fill(kKnobColor)
        .Corners(thumb / 2.f, CornerStyle::Rounded);

    if (parts.buttoned)
    {
        DrawSliderButton(list, parts.decrement, view.scale, false);
        DrawSliderButton(list, parts.increment, view.scale, true);
    }
}

/// How far along its track the pointer is, from 0 to 1, with the thumb's centre
/// under it so that where it was grabbed is where it stays.
float PointerFraction(const WidgetView &view, Point pointer)
{
    const Rect track = PlaceSlider(view).track;
    const float travel = ThumbTravel(track, view.scale);
    const float thumb = kSliderThumbWidth * view.scale;
    return travel > 0.f ? std::clamp((pointer.x - track.x - (thumb / 2.f)) / travel, 0.f, 1.f) : 0.f;
}

/// Which end button @p pointer is on: -1 for the one that takes away, 1 for the
/// one that adds, and 0 for neither.
int32_t ButtonUnder(const WidgetView &view, Point pointer)
{
    const SliderParts parts = PlaceSlider(view);
    if (!parts.buttoned)
    {
        return 0;
    }
    if (Covers(parts.decrement, pointer))
    {
        return -1;
    }
    return Covers(parts.increment, pointer) ? 1 : 0;
}

void DrawContinuousSlider(const WidgetView &view, DrawList &list)
{
    DrawSliderAt(view, list, ContinuousFraction(*view.node));
}

void DrawSteppedSlider(const WidgetView &view, DrawList &list)
{
    DrawSliderAt(view, list, StepFraction(*view.node));
}

/// Moves @p node to step @p step, saying whether it moved at all.
WidgetResponse MoveStep(Node &node, int32_t step)
{
    const int32_t clamped = std::clamp(step, 0, std::max(0, node.steps - 1));
    if (clamped == Step(node.value))
    {
        return WidgetResponse::Handled;
    }
    node.value = clamped;
    return WidgetResponse::Changed;
}

/// The step nearest the pointer, so a slider settles on one rather than resting
/// between two.
int32_t StepUnder(const WidgetView &view, const Node &node, Point pointer)
{
    const float along = PointerFraction(view, pointer) * static_cast<float>(std::max(0, node.steps - 1));
    return static_cast<int32_t>(std::lround(along));
}

WidgetResponse SteppedSliderInput(const WidgetView &view, Node &node, const WidgetEvent &event)
{
    switch (event.gesture)
    {
    case WidgetGesture::Press:
        if (const int32_t button = ButtonUnder(view, event.pointer); button != 0)
        {
            node.slidingTrack = false;
            return MoveStep(node, Step(node.value) + button);
        }
        node.slidingTrack = true;
        return MoveStep(node, StepUnder(view, node, event.pointer));
    case WidgetGesture::Drag:
        return node.slidingTrack ? MoveStep(node, StepUnder(view, node, event.pointer)) : WidgetResponse::Handled;
    case WidgetGesture::Action:
        if (event.action == UiAction::Left || event.action == UiAction::Right)
        {
            // Handled even at either end, so a slider being adjusted keeps focus
            // rather than handing it sideways.
            return MoveStep(node, Step(node.value) + (event.action == UiAction::Right ? 1 : -1));
        }
        return WidgetResponse::Ignored;
    case WidgetGesture::Release:
        node.slidingTrack = false;
        return WidgetResponse::Handled;
    case WidgetGesture::Activate:
        return WidgetResponse::Handled;
    case WidgetGesture::Wheel:
    case WidgetGesture::Type:
    case WidgetGesture::Edit:
    case WidgetGesture::Count:
        break;
    }
    return WidgetResponse::Ignored;
}

/// Moves @p node's value to @p value, within its range, saying whether it moved.
WidgetResponse MoveContinuous(Node &node, float value)
{
    const float clamped = std::clamp(value, node.range.min, node.range.max);
    if (clamped == Fraction(node.value))
    {
        return WidgetResponse::Handled;
    }
    node.value = clamped;
    return WidgetResponse::Changed;
}

WidgetResponse ContinuousSliderInput(const WidgetView &view, Node &node, const WidgetEvent &event)
{
    const float span = node.range.max - node.range.min;
    switch (event.gesture)
    {
    case WidgetGesture::Press:
        if (const int32_t button = ButtonUnder(view, event.pointer); button != 0)
        {
            node.slidingTrack = false;
            return MoveContinuous(node, Fraction(node.value) + (static_cast<float>(button) * node.range.step));
        }
        node.slidingTrack = true;
        return MoveContinuous(node, node.range.min + (PointerFraction(view, event.pointer) * span));
    case WidgetGesture::Drag:
        // A press that began on a button stays that button's, so the thumb does
        // not jump to a pointer sliding off it.
        return node.slidingTrack ? MoveContinuous(node, node.range.min + (PointerFraction(view, event.pointer) * span))
                                 : WidgetResponse::Handled;
    case WidgetGesture::Action:
        if (event.action == UiAction::Left || event.action == UiAction::Right)
        {
            const float step = event.action == UiAction::Right ? node.range.step : -node.range.step;
            // Handled even at the rail, so a slider at its end keeps focus
            // rather than handing it sideways mid-adjustment.
            return MoveContinuous(node, Fraction(node.value) + step);
        }
        return WidgetResponse::Ignored;
    case WidgetGesture::Release:
        node.slidingTrack = false;
        return WidgetResponse::Handled;
    case WidgetGesture::Activate:
        return WidgetResponse::Handled;
    case WidgetGesture::Wheel:
    case WidgetGesture::Type:
    case WidgetGesture::Edit:
    case WidgetGesture::Count:
        break;
    }
    return WidgetResponse::Ignored;
}

bool Scrolls(const Node &node, Axis axis)
{
    return node.style.enabledScrollBars[static_cast<std::size_t>(axis)];
}

/// @p point's coordinate along @p axis.
float Along(const Point &point, Axis axis)
{
    return axis == Axis::X ? point.x : point.y;
}

float &Along(Point &point, Axis axis)
{
    return axis == Axis::X ? point.x : point.y;
}

/// @p rect's length along @p axis.
float Length(const Rect &rect, Axis axis)
{
    return axis == Axis::X ? rect.width : rect.height;
}

/// Where @p rect starts along @p axis.
float Start(const Rect &rect, Axis axis)
{
    return axis == Axis::X ? rect.x : rect.y;
}

/// Where a scrolling node's bar sits, and what it stands for.
struct ScrollBarPlace
{
    Rect track;
    Rect thumb;
    float travel = 0.f; ///< device pixels the thumb may move along
    float hidden = 0.f; ///< device pixels of content out of sight
    bool shown = false;
};

ScrollBarPlace PlaceScrollBar(const WidgetView &view, Axis axis)
{
    const Node &node = *view.node;
    const Rect &rect = view.layout->rect;
    const float content = Along(view.layout->contentSize, axis);
    const float thickness = kScrollBarThickness * view.scale;
    // Each bar stops short of the other's, leaving the corner between them
    // clear rather than crossing it.
    const Axis other = axis == Axis::X ? Axis::Y : Axis::X;
    const float length = Length(rect, axis) - (Scrolls(node, other) ? thickness : 0.f);
    const float hidden = std::max(0.f, content - Length(rect, axis));

    ScrollBarPlace place;
    place.hidden = hidden;
    place.shown =
        Scrolls(node, axis) && (node.style.scrollBarVisibility == ScrollBarVisibility::Always ||
                                (node.style.scrollBarVisibility == ScrollBarVisibility::WhenNeeded && hidden > 0.f));
    if (!place.shown || content <= 0.f || length <= 0.f)
    {
        place.shown = false;
        return place;
    }

    const float thumbLength =
        std::clamp(length * (length / content), std::min(length, view.layout->scrollBarMinLength), length);
    place.travel = length - thumbLength;
    const float offset = Along(node.scrollOffset, axis) * view.scale;
    const float along = hidden > 0.f ? place.travel * std::clamp(offset / hidden, 0.f, 1.f) : 0.f;

    const bool vertical = axis == Axis::Y;
    place.track = vertical
                      ? Rect{.x = rect.x + rect.width - thickness, .y = rect.y, .width = thickness, .height = length}
                      : Rect{.x = rect.x, .y = rect.y + rect.height - thickness, .width = length, .height = thickness};
    place.thumb = vertical ? Rect{.x = place.track.x, .y = rect.y + along, .width = thickness, .height = thumbLength}
                           : Rect{.x = rect.x + along, .y = place.track.y, .width = thumbLength, .height = thickness};
    return place;
}

/// Moves @p node's content along @p axis by @p logical UI pixels, stopping where
/// its content does. @p glides says whether the style's smoothing applies, or
/// the content arrives at once.
WidgetResponse Scroll(const WidgetView &view, Node &node, Axis axis, float logical, bool glides = true)
{
    if (!Scrolls(node, axis) || view.scale <= 0.f)
    {
        return WidgetResponse::Ignored;
    }
    const float content = Along(view.layout->contentSize, axis);
    const float furthest = std::max(0.f, content - Length(view.layout->rect, axis)) / view.scale;
    float &target = Along(node.scrollTarget, axis);

    const float before = target;
    target = std::clamp(target + logical, 0.f, furthest);
    if (!glides || node.style.scrollSmoothing <= 0.f)
    {
        Along(node.scrollOffset, axis) = target;
    }
    return target == before ? WidgetResponse::Handled : WidgetResponse::Changed;
}

/// Scrolls @p node along @p axis as its thumb moving @p moved device pixels
/// would, which carries the content as much further as it is longer.
WidgetResponse DragBar(const WidgetView &view, Node &node, Axis axis, float moved)
{
    const ScrollBarPlace place = PlaceScrollBar(view, axis);
    if (!place.shown || place.travel <= 0.f)
    {
        return WidgetResponse::Handled;
    }
    // A dragged bar stays under the pointer unless the style says otherwise:
    // a thumb that lags behind the hand moving it feels broken rather than soft.
    return Scroll(view, node, axis, moved * (place.hidden / place.travel) / view.scale,
                  node.style.scrollBarDrag == ScrollBarDrag::Smoothed);
}

void DrawScroll(const WidgetView &view, DrawList &list)
{
    const float radius = kScrollBarThickness * view.scale / 2.f;
    for (const Axis axis : {Axis::X, Axis::Y})
    {
        const ScrollBarPlace place = PlaceScrollBar(view, axis);
        if (!place.shown)
        {
            continue;
        }
        list.Quad(place.track).Fill(kTrackColor).Corners(radius, CornerStyle::Rounded);
        list.Quad(place.thumb).Fill(kKnobColor).Corners(radius, CornerStyle::Rounded);
    }
}

bool ScrollClaims(const WidgetView &view, Point point)
{
    for (const Axis axis : {Axis::X, Axis::Y})
    {
        const ScrollBarPlace place = PlaceScrollBar(view, axis);
        if (place.shown && Covers(place.track, point))
        {
            return true;
        }
    }
    return false;
}

/// How far the wheel turned along @p axis. A node that scrolls only sideways
/// takes a plain turn sideways, since it has nowhere else to put it.
float WheelAlong(const Node &node, const Point &wheel, Axis axis)
{
    if (axis == Axis::Y)
    {
        return wheel.y;
    }
    return Scrolls(node, Axis::Y) ? wheel.x : wheel.x + wheel.y;
}

/// Takes hold of whichever bar @p pointer is on, and brings its thumb to the
/// pointer when the press landed on the track beside it.
WidgetResponse GrabBar(const WidgetView &view, Node &node, Point pointer)
{
    node.scrollGrab = ScrollGrab::None;
    for (const Axis axis : {Axis::X, Axis::Y})
    {
        const ScrollBarPlace place = PlaceScrollBar(view, axis);
        if (!place.shown || !Covers(place.track, pointer))
        {
            continue;
        }
        node.scrollGrab = axis == Axis::X ? ScrollGrab::Horizontal : ScrollGrab::Vertical;
        const float toThumb = Along(pointer, axis) - Start(place.thumb, axis) - (Length(place.thumb, axis) / 2.f);
        return DragBar(view, node, axis, toThumb);
    }
    return WidgetResponse::Handled;
}

WidgetResponse ScrollInput(const WidgetView &view, Node &node, const WidgetEvent &event)
{
    switch (event.gesture)
    {
    case WidgetGesture::Wheel:
    {
        // Scrolling away from the player reveals what is above it, as everywhere
        // else.
        WidgetResponse answer = WidgetResponse::Ignored;
        for (const Axis axis : {Axis::X, Axis::Y})
        {
            const float turned = WheelAlong(node, event.wheel, axis);
            if (turned != 0.f)
            {
                answer = std::max(answer, Scroll(view, node, axis, -turned * kWheelStep));
            }
        }
        return answer;
    }
    case WidgetGesture::Press:
        return GrabBar(view, node, event.pointer);
    case WidgetGesture::Drag:
        // Only a drag that began on the container: a press on a child captures
        // that child, which is what lets a button inside be clicked.
        switch (node.scrollGrab)
        {
        case ScrollGrab::Horizontal:
            return DragBar(view, node, Axis::X, event.pointerDelta.x);
        case ScrollGrab::Vertical:
            return DragBar(view, node, Axis::Y, event.pointerDelta.y);
        case ScrollGrab::None:
        case ScrollGrab::Count:
            break;
        }
        return std::max(Scroll(view, node, Axis::X, -event.pointerDelta.x / view.scale),
                        Scroll(view, node, Axis::Y, -event.pointerDelta.y / view.scale));
    case WidgetGesture::Release:
        node.scrollGrab = ScrollGrab::None;
        return WidgetResponse::Handled;
    case WidgetGesture::Action:
    {
        // Only while the container itself has focus, which a game asks for on a
        // list with nothing focusable in it — a log to read through. A container
        // holding its own controls is scrolled by focus moving between them.
        const bool vertical = event.action == UiAction::Down || event.action == UiAction::Up;
        const bool sideways = event.action == UiAction::Left || event.action == UiAction::Right;
        if (!view.focused || (!vertical && !sideways))
        {
            return WidgetResponse::Ignored;
        }
        const bool onwards = event.action == UiAction::Down || event.action == UiAction::Right;
        const WidgetResponse moved =
            Scroll(view, node, vertical ? Axis::Y : Axis::X, onwards ? kScrollKeyStep : -kScrollKeyStep);
        return moved == WidgetResponse::Ignored ? WidgetResponse::Ignored : WidgetResponse::Handled;
    }
    case WidgetGesture::Activate:
    case WidgetGesture::Type:
    case WidgetGesture::Edit:
    case WidgetGesture::Count:
        break;
    }
    return WidgetResponse::Ignored;
}

WidgetResponse ButtonInput(const WidgetView & /*view*/, Node & /*node*/, const WidgetEvent &event)
{
    // A button's activation is announced by the Ui itself; what it says here is
    // only that a press and a click are its own.
    switch (event.gesture)
    {
    case WidgetGesture::Press:
    case WidgetGesture::Release:
    case WidgetGesture::Activate:
        return WidgetResponse::Handled;
    case WidgetGesture::Drag:
    case WidgetGesture::Action:
    case WidgetGesture::Wheel:
    case WidgetGesture::Type:
    case WidgetGesture::Edit:
    case WidgetGesture::Count:
        break;
    }
    return WidgetResponse::Ignored;
}

} // namespace

Rect SliderThumbRect(const Node &node, const LayoutNode *layout, float scale, float fraction)
{
    if (layout == nullptr)
    {
        return {};
    }
    WidgetView view;
    view.node = &node;
    view.layout = layout;
    view.scale = scale;
    const Rect track = PlaceSlider(view).track;
    const float thumb = kSliderThumbWidth * scale;
    return Rect{.x = std::round(track.x + (ThumbTravel(track, scale) * std::clamp(fraction, 0.f, 1.f))),
                .y = track.y,
                .width = thumb,
                .height = track.height};
}

void RegisterBuiltinWidgets(WidgetRegistry &registry)
{
    // In the order BuiltinWidget names them, so a node can name one by its
    // enumerator rather than by remembering what came back.
    const uint32_t button = registry.Register(WidgetType{.input = &ButtonInput});
    const uint32_t toggle =
        registry.Register(WidgetType{.measure = &MeasureToggle, .draw = &DrawToggle, .input = &ToggleInput});
    const uint32_t slider = registry.Register(
        WidgetType{.measure = &MeasureSlider, .draw = &DrawContinuousSlider, .input = &ContinuousSliderInput});
    const uint32_t stepped = registry.Register(
        WidgetType{.measure = &MeasureSlider, .draw = &DrawSteppedSlider, .input = &SteppedSliderInput});
    const uint32_t scroll =
        registry.Register(WidgetType{.claims = &ScrollClaims, .draw = &DrawScroll, .input = &ScrollInput});
    const uint32_t field = registry.Register(TextFieldWidget());

    ASSISI_ASSERT(field == static_cast<uint32_t>(BuiltinWidget::TextField) &&
                      button == static_cast<uint32_t>(BuiltinWidget::Button) &&
                      toggle == static_cast<uint32_t>(BuiltinWidget::Toggle) &&
                      slider == static_cast<uint32_t>(BuiltinWidget::ContinuousSlider) &&
                      stepped == static_cast<uint32_t>(BuiltinWidget::SteppedSlider) &&
                      scroll == static_cast<uint32_t>(BuiltinWidget::Scroll),
                  "the built-in controls must be registered into an empty registry, in order");
}

} // namespace Assisi::Mondrian
