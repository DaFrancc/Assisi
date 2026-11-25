/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Layout.hpp>
#include <Assisi/Mondrian/NodeTree.hpp>
#include <Assisi/Mondrian/Widget.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <variant>

namespace Assisi::Mondrian
{
namespace
{

/// The built-ins' own look, which their parts wear until a theme or a screen
/// says otherwise.
constexpr Math::Color4<Math::ColorSpace::Srgb> kTrackColor{0.24f, 0.26f, 0.32f, 1.f};
constexpr Math::Color4<Math::ColorSpace::Srgb> kFillColor{0.90f, 0.20f, 0.10f, 1.f};
constexpr Math::Color4<Math::ColorSpace::Srgb> kKnobColor{0.92f, 0.93f, 0.96f, 1.f};

/// The ring every built-in control wears while it has the keys: how thick, how
/// far out from its edge, in UI pixels, and its colour.
constexpr float kFocusRingWidth = 3.f;
constexpr float kFocusRingGap = 3.f;
constexpr Math::Color4<Math::ColorSpace::Srgb> kFocusRingColor{1.f, 1.f, 1.f, 1.f};

/// Sizes in UI pixels.
constexpr float kToggleWidth = 56.f;
constexpr float kToggleHeight = 28.f;
constexpr float kSliderWidth = 220.f;
constexpr float kSliderHeight = 28.f;
constexpr float kSliderTrackHeight = 6.f;
constexpr float kSliderThumbWidth = 16.f;

/// How round a part's corners are: half its shorter side, which makes a pill
/// of a long part and a circle of a square one.
constexpr Length kPillCorners = Percent(50.f);

/// How far one notch of the wheel scrolls, in UI pixels, and how far one
/// press of a key does.
constexpr float kWheelStep = 48.f;
constexpr float kScrollKeyStep = 40.f;

/// How wide the bar showing where a scrolling node is sits, in UI pixels.
constexpr float kScrollBarThickness = 6.f;

/// Each built-in's parts, by where they sit in its list.
enum class TogglePart : uint8_t
{
    Track,
    Knob,
    Count_
};

enum class SliderPart : uint8_t
{
    Track,
    Fill,
    Thumb,
    Count_
};

/// A scroll's two bars, across first, each a track and a thumb.
enum class ScrollPart : uint8_t
{
    AcrossTrack,
    AcrossThumb,
    DownTrack,
    DownThumb,
    Count_
};

template <typename Part> constexpr std::size_t At(Part part)
{
    return static_cast<std::size_t>(part);
}

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

/// Whether @p point is on @p rect, which a bar is pressed by.
bool Covers(const Rect &rect, Point point)
{
    return point.x >= rect.x && point.x <= rect.x + rect.width && point.y >= rect.y && point.y <= rect.y + rect.height;
}

/// What @p view's padding leaves of its control's box: where its parts go.
Rect ContentBox(const WidgetView &view)
{
    const Rect &rect = view.layout->rect;
    const Insets &padding = view.layout->padding;
    return {.x = rect.x + padding.left,
            .y = rect.y + padding.top,
            .width = std::max(0.f, rect.width - padding.left - padding.right),
            .height = std::max(0.f, rect.height - padding.top - padding.bottom)};
}

/// Where a part @p height tall sits to be centred down @p box.
float CentredDown(const Rect &box, float height)
{
    return box.y + ((box.height - height) / 2.f);
}

// -----------------------------------------------------------------------------
// Default looks
// -----------------------------------------------------------------------------

/// A part's resting look: @p width by @p height, filled with @p fill, its
/// corners rounded to a pill.
StylePatch PillLook(Sizing width, Sizing height, const Math::Color4<Math::ColorSpace::Srgb> &fill)
{
    Style style;
    style.sizing = {width, height};
    style.background = fill;
    style.cornerRadius = kPillCorners;
    style.cornerStyle = CornerStyle::Rounded;
    StylePatch look;
    for (const StyleProperty property : {StyleProperty::Width, StyleProperty::Height, StyleProperty::Background,
                                         StyleProperty::CornerRadius, StyleProperty::CornerStyle})
    {
        look.Take(property, style);
    }
    return look;
}

/// A look for @p state that changes only the background, to @p fill.
StylePatch FilledWhen(StyleState state, const Math::Color4<Math::ColorSpace::Srgb> &fill)
{
    Style style;
    style.background = fill;
    StylePatch look;
    look.state = state;
    look.Take(StyleProperty::Background, style);
    return look;
}

/// A ring around a control while it has the keys.
DefaultLook FocusRinged()
{
    Style ring;
    ring.outlineWidth = Px(kFocusRingWidth);
    ring.outlineGap = Px(kFocusRingGap);
    ring.outlineColor = kFocusRingColor;
    StylePatch focused;
    focused.state = StyleState::Focused;
    focused.Take(StyleProperty::OutlineWidth, ring);
    focused.Take(StyleProperty::OutlineGap, ring);
    focused.Take(StyleProperty::OutlineColor, ring);
    return DefaultLook{.resting = {}, .states = {focused}};
}

/// How every built-in control looks of itself.
const DefaultLook &ControlLook()
{
    static const DefaultLook look = FocusRinged();
    return look;
}

/// A toggle's parts: a track that fills while it is on, and the knob that
/// slides along it.
std::span<const WidgetPart> ToggleParts()
{
    static const std::array<WidgetPart, At(TogglePart::Count_)> parts{{
        {.name = "track",
         .look = {.resting = PillLook(Sizing::Grow(), Sizing::Grow(), kTrackColor),
                  .states = {FilledWhen(StyleState::On, kFillColor)}}},
        {.name = "knob",
         .look = {.resting = PillLook(Sizing::Fixed(Px(kToggleHeight)), Sizing::Fixed(Px(kToggleHeight)), kKnobColor),
                  .states = {}}},
    }};
    return parts;
}

/// A slider's parts: its track, the share of it behind the thumb, and the thumb.
std::span<const WidgetPart> SliderParts()
{
    static const std::array<WidgetPart, At(SliderPart::Count_)> parts{{
        {.name = "track",
         .look = {.resting = PillLook(Sizing::Grow(), Sizing::Fixed(Px(kSliderTrackHeight)), kTrackColor),
                  .states = {}}},
        // As wide as the slider makes it.
        {.name = "fill",
         .look = {.resting = PillLook(Sizing::Fixed(Px(0.f)), Sizing::Fixed(Px(kSliderTrackHeight)), kFillColor),
                  .states = {}}},
        {.name = "thumb",
         .look = {.resting = PillLook(Sizing::Fixed(Px(kSliderThumbWidth)), Sizing::Grow(), kKnobColor),
                  .states = {}}},
    }};
    return parts;
}

/// A scroll's parts: a track and a thumb for each bar, sized by the scroll.
std::span<const WidgetPart> ScrollParts()
{
    static const std::array<WidgetPart, At(ScrollPart::Count_)> parts{{
        {.name = "track", .look = {.resting = PillLook(Sizing::Fit(), Sizing::Fit(), kTrackColor), .states = {}}},
        {.name = "thumb", .look = {.resting = PillLook(Sizing::Fit(), Sizing::Fit(), kKnobColor), .states = {}}},
        {.name = "track", .look = {.resting = PillLook(Sizing::Fit(), Sizing::Fit(), kTrackColor), .states = {}}},
        {.name = "thumb", .look = {.resting = PillLook(Sizing::Fit(), Sizing::Fit(), kKnobColor), .states = {}}},
    }};
    return parts;
}

// -----------------------------------------------------------------------------
// Toggle
// -----------------------------------------------------------------------------

Point MeasureToggle(const Node & /*node*/, void * /*context*/)
{
    return {.x = kToggleWidth, .y = kToggleHeight};
}

/// The track down the middle, and the knob at the end the toggle is switched to.
void PlaceToggle(const WidgetView &view, std::span<PartPlace> parts)
{
    const Rect box = ContentBox(view);
    Rect &track = parts[At(TogglePart::Track)].rect;
    track.x = box.x;
    track.y = CentredDown(box, track.height);
    Rect &knob = parts[At(TogglePart::Knob)].rect;
    knob.x = Switched(view.node->value) ? box.x + box.width - knob.width : box.x;
    knob.y = CentredDown(box, knob.height);
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

// -----------------------------------------------------------------------------
// Sliders
// -----------------------------------------------------------------------------

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

/// How far along its box a slider's thumb, @p thumb device pixels wide, may move.
float ThumbTravel(const Rect &box, float thumb)
{
    return std::max(0.f, box.width - thumb);
}

/// The track down the middle of the box, the thumb @p fraction of the way along
/// it, and the fill from the start up to the thumb's middle.
void PlaceSliderAt(const WidgetView &view, std::span<PartPlace> parts, float fraction)
{
    const Rect box = ContentBox(view);
    Rect &track = parts[At(SliderPart::Track)].rect;
    Rect &fill = parts[At(SliderPart::Fill)].rect;
    Rect &thumb = parts[At(SliderPart::Thumb)].rect;
    const float travel = ThumbTravel(box, thumb.width);

    track.x = box.x;
    track.y = CentredDown(box, track.height);
    fill.x = box.x;
    fill.y = CentredDown(box, fill.height);
    fill.width = std::round((travel * fraction) + (thumb.width / 2.f));
    thumb.x = std::round(box.x + (travel * fraction));
    thumb.y = CentredDown(box, thumb.height);
}

void PlaceContinuousSlider(const WidgetView &view, std::span<PartPlace> parts)
{
    PlaceSliderAt(view, parts, ContinuousFraction(*view.node));
}

void PlaceSteppedSlider(const WidgetView &view, std::span<PartPlace> parts)
{
    PlaceSliderAt(view, parts, StepFraction(*view.node));
}

/// How far along its box the pointer is, from 0 to 1, with the thumb's middle
/// under it so that where it was grabbed is where it stays.
float PointerFraction(const WidgetView &view, Point pointer)
{
    const Rect box = ContentBox(view);
    const float thumb = view.parts[At(SliderPart::Thumb)].width;
    const float travel = ThumbTravel(box, thumb);
    return travel > 0.f ? std::clamp((pointer.x - box.x - (thumb / 2.f)) / travel, 0.f, 1.f) : 0.f;
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
    case WidgetGesture::Drag:
        return MoveStep(node, StepUnder(view, node, event.pointer));
    case WidgetGesture::Action:
        if (event.action == UiAction::Left || event.action == UiAction::Right)
        {
            // Handled even at either end, so a slider being adjusted keeps focus
            // rather than handing it sideways.
            return MoveStep(node, Step(node.value) + (event.action == UiAction::Right ? 1 : -1));
        }
        return WidgetResponse::Ignored;
    case WidgetGesture::Release:
    case WidgetGesture::Activate:
        return WidgetResponse::Handled;
    case WidgetGesture::Wheel:
    case WidgetGesture::Type:
    case WidgetGesture::Edit:
    case WidgetGesture::Count_:
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
    case WidgetGesture::Drag:
        return MoveContinuous(node, node.range.min + (PointerFraction(view, event.pointer) * span));
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
    case WidgetGesture::Activate:
        return WidgetResponse::Handled;
    case WidgetGesture::Wheel:
    case WidgetGesture::Type:
    case WidgetGesture::Edit:
    case WidgetGesture::Count_:
        break;
    }
    return WidgetResponse::Ignored;
}

// -----------------------------------------------------------------------------
// Scroll
// -----------------------------------------------------------------------------

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

/// Each bar's track and thumb where the scroll offset puts them, and neither
/// for a bar that does not show.
void PlaceScroll(const WidgetView &view, std::span<PartPlace> parts)
{
    for (const Axis axis : {Axis::X, Axis::Y})
    {
        const ScrollBarPlace place = PlaceScrollBar(view, axis);
        const bool across = axis == Axis::X;
        PartPlace &track = parts[At(across ? ScrollPart::AcrossTrack : ScrollPart::DownTrack)];
        PartPlace &thumb = parts[At(across ? ScrollPart::AcrossThumb : ScrollPart::DownThumb)];
        track = PartPlace{.rect = place.track, .shown = place.shown};
        thumb = PartPlace{.rect = place.thumb, .shown = place.shown};
    }
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
        case ScrollGrab::Count_:
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
    case WidgetGesture::Count_:
        break;
    }
    return WidgetResponse::Ignored;
}

// -----------------------------------------------------------------------------
// Button
// -----------------------------------------------------------------------------

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
    case WidgetGesture::Count_:
        break;
    }
    return WidgetResponse::Ignored;
}

/// Every built-in control's type, by BuiltinWidget.
using BuiltinTypeTable = std::array<WidgetType, static_cast<std::size_t>(BuiltinWidget::Count_)>;

BuiltinTypeTable MakeBuiltinTypes()
{
    WidgetType field = TextFieldWidget();
    field.look = &ControlLook();
    const WidgetType selectable = field;
    field.name = "text_field";
    return {{
        WidgetType{},
        WidgetType{.name = "button", .look = &ControlLook(), .input = &ButtonInput},
        WidgetType{.name = "toggle",
                   .parts = ToggleParts(),
                   .look = &ControlLook(),
                   .measure = &MeasureToggle,
                   .place = &PlaceToggle,
                   .input = &ToggleInput},
        WidgetType{.name = "slider",
                   .parts = SliderParts(),
                   .look = &ControlLook(),
                   .measure = &MeasureSlider,
                   .place = &PlaceContinuousSlider,
                   .input = &ContinuousSliderInput},
        WidgetType{.name = "stepped_slider",
                   .parts = SliderParts(),
                   .look = &ControlLook(),
                   .measure = &MeasureSlider,
                   .place = &PlaceSteppedSlider,
                   .input = &SteppedSliderInput},
        WidgetType{.name = "scroll",
                   .parts = ScrollParts(),
                   .look = &ControlLook(),
                   .place = &PlaceScroll,
                   .input = &ScrollInput},
        field,
        selectable,
    }};
}

/// The one list of the built-in controls: what is registered, what a file
/// calls each, and which parts each has. Made once and kept, so a span or a
/// name taken from it never outlives it.
const BuiltinTypeTable &BuiltinTypes()
{
    static const BuiltinTypeTable types = MakeBuiltinTypes();
    return types;
}

} // namespace

void RegisterBuiltinWidgets(WidgetRegistry &registry)
{
    // In the order BuiltinWidget names them, so a node can name one by its
    // enumerator rather than by remembering what came back.
    const BuiltinTypeTable &types = BuiltinTypes();
    for (std::size_t index = 1; index < types.size(); ++index)
    {
        const uint32_t id = registry.Register(types[index]);
        ASSISI_ASSERT(id == index, "the built-in controls must be registered into an empty registry, in order");
    }
}

std::string_view BuiltinWidgetName(BuiltinWidget widget)
{
    const std::size_t index = static_cast<std::size_t>(widget);
    return index < static_cast<std::size_t>(BuiltinWidget::Count_) ? BuiltinTypes()[index].name : std::string_view{};
}

std::vector<std::string_view> BuiltinPartNames(BuiltinWidget widget)
{
    std::vector<std::string_view> names;
    const std::size_t index = static_cast<std::size_t>(widget);
    if (index >= static_cast<std::size_t>(BuiltinWidget::Count_))
    {
        return names;
    }
    for (const WidgetPart &part : BuiltinTypes()[index].parts)
    {
        if (std::ranges::find(names, part.name) == names.end())
        {
            names.push_back(part.name);
        }
    }
    return names;
}

} // namespace Assisi::Mondrian
