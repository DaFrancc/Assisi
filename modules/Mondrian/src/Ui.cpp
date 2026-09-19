/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Mondrian/Draw.hpp>
#include <Assisi/Mondrian/HitTest.hpp>
#include <Assisi/Mondrian/Style.hpp>

#include <Assisi/Core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace Assisi::Mondrian
{
namespace
{

/// The ring around the focused node while keys were used last: its thickness,
/// and the space between it and the node, in logical pixels.
constexpr float kFocusRingWidth = 3.f;
constexpr float kFocusRingGap = 3.f;
constexpr Math::Color4<Math::ColorSpace::Srgb> kFocusRingColor{1.f, 1.f, 1.f, 1.f};

/// The actions that move focus, which a focused control may take instead.
constexpr std::array kMoveActions{UiAction::Up,    UiAction::Down, UiAction::Left,
                                  UiAction::Right, UiAction::Next, UiAction::Previous};

/// One gesture at @p pointer, with nothing else to say.
WidgetEvent PointerGesture(WidgetGesture gesture, Point pointer)
{
    WidgetEvent event;
    event.gesture = gesture;
    event.pointer = pointer;
    return event;
}

WidgetEvent DragGesture(Point pointer, Point delta)
{
    WidgetEvent event = PointerGesture(WidgetGesture::Drag, pointer);
    event.pointerDelta = delta;
    return event;
}

WidgetEvent WheelGesture(Point pointer, Point wheel)
{
    WidgetEvent event = PointerGesture(WidgetGesture::Wheel, pointer);
    event.wheel = wheel;
    return event;
}

WidgetEvent ActionGesture(UiAction action)
{
    WidgetEvent event;
    event.gesture = WidgetGesture::Action;
    event.action = action;
    return event;
}

// The sample screen shown while no real screen exists: a panel centred on the
// screen with a picture and title, a wrapped paragraph and a row of buttons,
// and a badge floating over its corner. It exercises every kind of sizing, so a
// capture at two window sizes shows at a glance whether layout reflows.

constexpr Math::Color4<Math::ColorSpace::Srgb> kPanelColor{0.10f, 0.11f, 0.14f, 0.94f};
constexpr Math::Color4<Math::ColorSpace::Srgb> kPanelBorder{0.34f, 0.38f, 0.48f, 1.f};
constexpr Math::Color4<Math::ColorSpace::Srgb> kAccent{0.90f, 0.20f, 0.10f, 1.f};
constexpr Math::Color4<Math::ColorSpace::Srgb> kWhite{1.f, 1.f, 1.f, 1.f};
constexpr Math::Color4<Math::ColorSpace::Srgb> kBodyText{0.80f, 0.82f, 0.86f, 1.f};
constexpr Rect kWholeTexture{.x = 0.f, .y = 0.f, .width = 1.f, .height = 1.f};

/// Lengths in logical pixels.
constexpr float kPanelWidth = 720.f;
constexpr float kPanelPadding = 32.f;
constexpr float kPanelGap = 20.f;
constexpr float kPanelRadius = 16.f;
constexpr float kPanelBorderWidth = 2.f;
constexpr float kPictureSide = 72.f;
constexpr float kPictureRadius = 12.f;
constexpr float kTitleSize = 48.f;
constexpr float kBodySize = 24.f;
constexpr float kButtonSize = 28.f;
constexpr float kButtonRadius = 10.f;
constexpr Padding kButtonPadding{.left = 28.f, .top = 10.f, .right = 28.f, .bottom = 10.f};
constexpr SliderRange kSampleSliderRange{.min = 0.f, .max = 100.f, .step = 5.f};
constexpr float kSampleSliderStart = 60.f;
constexpr SliderRange kSampleStepRange{.min = 0.f, .max = 3.f, .step = 1.f};
constexpr int32_t kSampleSliderSteps = 4;
constexpr int32_t kSampleSliderStep = 1;
constexpr float kSampleListHeight = 150.f;
constexpr float kSampleScrollSmoothing = 0.12f;
constexpr Math::Color4<Math::ColorSpace::Srgb> kListColor{0.06f, 0.07f, 0.09f, 1.f};
constexpr float kBadgeSize = 20.f;
constexpr float kBadgeRadius = 16.f;
constexpr Padding kBadgePadding{.left = 14.f, .top = 4.f, .right = 14.f, .bottom = 4.f};

/// The paragraph, spelled in bytes so the source file's encoding cannot change
/// it. It reads: "A retained tree of boxes, laid out in passes: widths first,
/// text wraps to them, then heights and positions. Resize the window and
/// everything reflows, with every edge on a whole pixel. Crème brûlée."
/// (A hex escape runs on through any hex digit, hence the split before an e.)
constexpr std::string_view kParagraph =
    "A retained tree of boxes, laid out in passes: widths first, text wraps to them, then heights and "
    "positions. Resize the window and everything reflows, with every edge on a whole pixel. "
    "Cr\xC3\xA8me br\xC3\xBBl\xC3\xA9"
    "e.";

NodeId Add(NodeTree &tree, NodeId parent, std::string_view name, const Style &style)
{
    const NodeId id = tree.Create(parent, name);
    tree.SetStyle(id, style);
    return id;
}

NodeId AddText(NodeTree &tree, NodeId parent, std::string_view name, const Style &style, std::string_view text)
{
    const NodeId id = Add(tree, parent, name, style);
    tree.SetText(id, text);
    return id;
}

/// A button: a label with padding around it, over @p background.
void AddButton(NodeTree &tree, NodeId row, std::string_view label, const Style &look)
{
    Style style = look;
    style.padding = kButtonPadding;
    style.textSize = kButtonSize;
    const NodeId id = AddText(tree, row, label, style, label);
    tree.SetFocusable(id, true);
    tree.SetBehaviour(id, static_cast<uint32_t>(BuiltinWidget::Button));
}

/// Builds the sample screen under @p tree's root, returning its picture.
NodeId BuildSampleScreen(NodeTree &tree)
{
    Style root;
    root.childAlign = {Alignment::Center, Alignment::Center};
    tree.SetStyle(tree.Root(), root);

    Style panel;
    panel.sizing = {Sizing::Fixed(kPanelWidth), Sizing::Fit()};
    panel.direction = Direction::Column;
    panel.padding = Padding::All(kPanelPadding);
    panel.gap = kPanelGap;
    panel.background = kPanelColor;
    panel.borderWidth = kPanelBorderWidth;
    panel.borderColor = kPanelBorder;
    panel.cornerRadius = kPanelRadius;
    panel.cornerStyle = CornerStyle::Rounded;
    const NodeId panelId = Add(tree, tree.Root(), "panel", panel);
    tree.SetBlocksPointer(panelId, true);

    Style header;
    header.sizing = {Sizing::Grow(), Sizing::Fit()};
    header.gap = kPanelGap;
    header.childAlign = {Alignment::Start, Alignment::Center};
    const NodeId headerId = Add(tree, panelId, "header", header);

    Style picture;
    picture.sizing = {Sizing::Fixed(kPictureSide), Sizing::Fixed(kPictureSide)};
    picture.cornerRadius = kPictureRadius;
    picture.cornerStyle = CornerStyle::Rounded;
    const NodeId pictureId = Add(tree, headerId, "picture", picture);
    tree.SetImage(pictureId, kWhiteTexture, kWholeTexture);

    Style title;
    title.sizing = {Sizing::Grow(), Sizing::Fit()};
    title.textSize = kTitleSize;
    AddText(tree, headerId, "title", title, "Mondrian");

    Style body;
    body.sizing = {Sizing::Grow(), Sizing::Fit()};
    body.textSize = kBodySize;
    body.textColor = kBodyText;
    AddText(tree, panelId, "body", body, kParagraph);

    Style buttons;
    buttons.sizing = {Sizing::Grow(), Sizing::Fit()};
    buttons.gap = kPanelGap;
    buttons.childAlign = {Alignment::End, Alignment::Center};
    const NodeId buttonsId = Add(tree, panelId, "buttons", buttons);

    Style outlined;
    outlined.borderWidth = kPanelBorderWidth;
    outlined.borderColor = kWhite;
    outlined.cornerRadius = kButtonRadius;
    outlined.cornerStyle = CornerStyle::Rounded;
    AddButton(tree, buttonsId, "Quit", outlined);

    Style filled;
    filled.background = kAccent;
    filled.cornerRadius = kButtonRadius;
    filled.cornerStyle = CornerStyle::Cut;
    AddButton(tree, buttonsId, "Resume", filled);

    Style badge;
    badge.padding = kBadgePadding;
    badge.background = kAccent;
    badge.cornerRadius = kBadgeRadius;
    badge.cornerStyle = CornerStyle::Rounded;
    badge.textSize = kBadgeSize;
    badge.floating.enabled = true;
    badge.floating.anchor = {Alignment::End, Alignment::Start};
    badge.floating.attach = {Alignment::Center, Alignment::Center};
    AddText(tree, panelId, "badge", badge, "NEW");

    return pictureId;
}

} // namespace

Ui::Ui()
{
    // Before the screen, so the nodes it makes can name the built-ins.
    RegisterBuiltinWidgets(_tree.Widgets());
    _picture = BuildSampleScreen(_tree);
    AddSampleControls();
}

void Ui::AddSampleControls()
{
    const NodeId panel = _tree.Find("panel");

    Style row;
    row.sizing = {Sizing::Grow(), Sizing::Fit()};
    row.gap = kPanelGap;
    row.childAlign = {Alignment::Start, Alignment::Center};
    const NodeId controls = Add(_tree, panel, "controls", row);
    AddToggle(controls, true);
    const ContinuousSliderId volume = AddContinuousSlider(controls, kSampleSliderRange, kSampleSliderStart);
    SetButtons(volume, SliderButtons::Shown);
    AddSteppedSlider(controls, kSampleStepRange, kSampleSliderSteps, kSampleSliderStep);

    Style list;
    list.sizing = {Sizing::Grow(), Sizing::Fixed(kSampleListHeight)};
    list.direction = Direction::Column;
    list.background = kListColor;
    list.cornerRadius = kButtonRadius;
    list.cornerStyle = CornerStyle::Rounded;
    list.scrollBarVisibility = ScrollBarVisibility::WhenNeeded;
    list.scrollSmoothing = kSampleScrollSmoothing;
    const NodeId scroller = AddScroll(panel, list, {false, true});
    _tree.SetName(scroller, "list");

    Style entry;
    entry.sizing = {Sizing::Grow(), Sizing::Fit()};
    entry.padding = kButtonPadding;
    entry.textSize = kButtonSize;
    for (const std::string_view label : {"One", "Two", "Three", "Four", "Five"})
    {
        const NodeId id = AddText(_tree, scroller, label, entry, label);
        _tree.SetFocusable(id, true);
        _tree.SetBehaviour(id, static_cast<uint32_t>(BuiltinWidget::Button));
    }
}

ButtonId Ui::AddButton(NodeId parent, std::string_view label)
{
    const NodeId node = _tree.Create(parent, label);
    _tree.SetText(node, label);
    _tree.SetBehaviour(node, static_cast<uint32_t>(BuiltinWidget::Button));
    _tree.SetFocusable(node, true);
    return {.node = node};
}

ToggleId Ui::AddToggle(NodeId parent, bool on)
{
    const NodeId node = _tree.Create(parent);
    _tree.SetBehaviour(node, static_cast<uint32_t>(BuiltinWidget::Toggle));
    _tree.SetFocusable(node, true);
    _tree.SetValue(node, on);
    return {.node = node};
}

ContinuousSliderId Ui::AddContinuousSlider(NodeId parent, SliderRange range, float value)
{
    const NodeId node = _tree.Create(parent);
    _tree.SetBehaviour(node, static_cast<uint32_t>(BuiltinWidget::ContinuousSlider));
    _tree.SetFocusable(node, true);
    _tree.SetRange(node, range);
    _tree.SetValue(node, std::clamp(value, range.min, range.max));
    return {.node = node};
}

SteppedSliderId Ui::AddSteppedSlider(NodeId parent, SliderRange range, int32_t steps, int32_t step)
{
    const NodeId node = _tree.Create(parent);
    _tree.SetBehaviour(node, static_cast<uint32_t>(BuiltinWidget::SteppedSlider));
    _tree.SetFocusable(node, true);
    _tree.SetRange(node, range);
    _tree.SetSteps(node, std::max(1, steps));
    _tree.SetValue(node, std::clamp(step, 0, std::max(0, steps - 1)));
    return {.node = node};
}

void Ui::SetRange(ContinuousSliderId slider, SliderRange range)
{
    _tree.SetRange(slider.node, range);
    SetValue(slider, GetValue(slider));
}

void Ui::SetRange(SteppedSliderId slider, SliderRange range)
{
    _tree.SetRange(slider.node, range);
}

void Ui::SetButtons(ContinuousSliderId slider, SliderButtons buttons)
{
    _tree.SetSliderButtons(slider.node, buttons);
}

void Ui::SetButtons(SteppedSliderId slider, SliderButtons buttons)
{
    _tree.SetSliderButtons(slider.node, buttons);
}

SliderRange Ui::GetRange(ContinuousSliderId slider) const
{
    const Node *node = _tree.Get(slider.node);
    return node != nullptr ? node->range : SliderRange{};
}

SliderRange Ui::GetRange(SteppedSliderId slider) const
{
    const Node *node = _tree.Get(slider.node);
    return node != nullptr ? node->range : SliderRange{};
}

float Ui::GetFraction(ContinuousSliderId slider) const
{
    const SliderRange range = GetRange(slider);
    const float span = range.max - range.min;
    return span > 0.f ? std::clamp((GetValue(slider) - range.min) / span, 0.f, 1.f) : 0.f;
}

float Ui::GetFraction(SteppedSliderId slider) const
{
    const int32_t steps = GetSteps(slider);
    return steps > 1 ? static_cast<float>(GetValue(slider)) / static_cast<float>(steps - 1) : 0.f;
}

int32_t Ui::GetSteps(SteppedSliderId slider) const
{
    const Node *node = _tree.Get(slider.node);
    return node != nullptr ? node->steps : 0;
}

float Ui::GetStepValue(SteppedSliderId slider, int32_t step) const
{
    const SliderRange range = GetRange(slider);
    const int32_t steps = GetSteps(slider);
    if (steps <= 1)
    {
        return range.min;
    }
    const float part = static_cast<float>(std::clamp(step, 0, steps - 1)) / static_cast<float>(steps - 1);
    return range.min + (part * (range.max - range.min));
}

float Ui::GetValue(ContinuousSliderId slider) const
{
    const Node *node = _tree.Get(slider.node);
    return node != nullptr ? Held<float>(node->value) : 0.f;
}

int32_t Ui::GetValue(SteppedSliderId slider) const
{
    const Node *node = _tree.Get(slider.node);
    return node != nullptr ? Held<int32_t>(node->value) : 0;
}

Rect Ui::GetThumbRect(ContinuousSliderId slider) const
{
    const Node *node = _tree.Get(slider.node);
    return node != nullptr ? SliderThumbRect(*node, _layout.Get(slider.node), _layout.scale, GetFraction(slider))
                           : Rect{};
}

Rect Ui::GetThumbRect(SteppedSliderId slider) const
{
    const Node *node = _tree.Get(slider.node);
    return node != nullptr ? SliderThumbRect(*node, _layout.Get(slider.node), _layout.scale, GetFraction(slider))
                           : Rect{};
}

NodeId Ui::AddScroll(NodeId parent, const Style &style, std::array<bool, kAxisCount> axes)
{
    const NodeId node = _tree.Create(parent);
    Style scrolling = style;
    scrolling.enabledScrollBars = axes;
    _tree.SetStyle(node, scrolling);
    _tree.SetBehaviour(node, static_cast<uint32_t>(BuiltinWidget::Scroll));
    // Its own background is what a drag scrolls, and a press on it is the UI's
    // rather than the game's.
    _tree.SetBlocksPointer(node, true);
    return node;
}

void Ui::SetValue(ContinuousSliderId slider, float value)
{
    const SliderRange range = GetRange(slider);
    if (_interaction.pressed != slider.node)
    {
        _tree.SetValue(slider.node, std::clamp(value, range.min, range.max));
    }
}

void Ui::SetValue(ToggleId toggle, bool on)
{
    if (_interaction.pressed != toggle.node)
    {
        _tree.SetValue(toggle.node, on);
    }
}

void Ui::SetValue(SteppedSliderId slider, int32_t step)
{
    const Node *node = _tree.Get(slider.node);
    if (node != nullptr && _interaction.pressed != slider.node)
    {
        _tree.SetValue(slider.node, std::clamp(step, 0, std::max(0, node->steps - 1)));
    }
}

void Ui::SetPlaceholderTexture(TextureId texture)
{
    _tree.SetImage(_picture, texture, kWholeTexture);
}

InputResult Ui::ProcessInput(const UiInput &input)
{
    ASSISI_ASSERT(_nextStep == FrameStep::AwaitingInput, "Ui::ProcessInput called twice without a Sync between");
    _nextStep = FrameStep::AwaitingSync;

    const InputResult result = Interact(input);
    AdvanceScrolling(std::max(0.0, input.time - _lastTime));
    _lastTime = input.time;
    if (_interaction.activated)
    {
        Dispatch(_interaction.activated, PointerGesture(WidgetGesture::Activate, _lastPointer));
    }
    Announce();
    return result;
}

void Ui::Announce()
{
    if (_events == nullptr)
    {
        _changed.clear();
        return;
    }
    if (const Node *activated = _tree.Get(_interaction.activated); activated != nullptr && activated->onActivate)
    {
        activated->onActivate(*_events);
    }
    for (const NodeId id : _changed)
    {
        if (const Node *node = _tree.Get(id); node != nullptr && node->onChange)
        {
            node->onChange(*_events, node->value);
        }
    }
    _changed.clear();
    if (_interaction.backPressed)
    {
        _events->Push(UiBack{});
    }
}

void Ui::AdvanceScrolling(double seconds)
{
    // Within half a logical pixel is arrived: the rest would creep for frames
    // nobody can see, and layout snaps to whole pixels anyway.
    constexpr float kSettled = 0.5f;

    for (uint32_t index = 0; index < _tree.Slots().size(); ++index)
    {
        const NodeId id = _tree.IdOf(index);
        Node *node = _tree.Editable(id);
        if (node == nullptr || node->style.scrollSmoothing <= 0.f)
        {
            continue;
        }
        const float part = std::min(1.f, static_cast<float>(seconds) / node->style.scrollSmoothing);
        const auto approach = [part](float offset, float target)
        { return std::abs(target - offset) <= kSettled ? target : offset + ((target - offset) * part); };

        node->scrollOffset.x = approach(node->scrollOffset.x, node->scrollTarget.x);
        node->scrollOffset.y = approach(node->scrollOffset.y, node->scrollTarget.y);
    }
}

WidgetResponse Ui::Dispatch(NodeId id, const WidgetEvent &event)
{
    const WidgetType *widget = _tree.WidgetOf(id);
    const LayoutNode *placed = _layout.Get(id);
    Node *node = _tree.Editable(id);
    if (widget == nullptr || widget->input == nullptr || placed == nullptr || node == nullptr)
    {
        return WidgetResponse::Ignored;
    }

    const WidgetView view{.node = node,
                          .layout = placed,
                          .context = widget->context,
                          .scale = _layout.scale,
                          .id = id,
                          .focused = _interaction.focused == id,
                          .pressed = _interaction.pressed == id,
                          .hovered = _interaction.hovered == id};
    const WidgetResponse response = widget->input(view, *node, event);
    if (response == WidgetResponse::Changed && std::ranges::find(_changed, id) == _changed.end())
    {
        _changed.push_back(id);
    }
    return response;
}

bool Ui::DispatchWheel(NodeId hit, const UiInput &input)
{
    const WidgetEvent event = WheelGesture(input.pointer, input.wheel);
    // Up the tree from whatever the pointer is over: a wheel over a button
    // inside a list scrolls the list, as it does everywhere else.
    for (NodeId id = hit; id; id = _tree.Get(id)->parent)
    {
        if (Dispatch(id, event) != WidgetResponse::Ignored)
        {
            return true;
        }
    }
    return false;
}

InputResult Ui::Interact(const UiInput &input)
{
    InputResult result;
    Interaction &now = _interaction;
    now.activated = {};
    now.backPressed = false;

    // A focused node that has gone, hidden or been disabled hands focus on, so
    // the keys still have somewhere to act. Not before the first layout, which
    // has placed nothing yet: focus set ahead of it would be lost.
    const bool laidOut = _layout.scale > 0.f;
    if (laidOut && now.focused && !CanFocus(_tree, _layout, now.focused))
    {
        now.focused = FirstFocusable(_tree, _layout);
    }

    const Point delta{.x = input.pointer.x - _lastPointer.x, .y = input.pointer.y - _lastPointer.y};
    const bool moved = delta.x != 0.f || delta.y != 0.f;
    _lastPointer = input.pointer;
    if (input.grant == InputGrant::Nothing || input.pointerClaimed)
    {
        now.hovered = {};
        now.pressed = {};
    }
    else
    {
        const NodeId hit = HitTest(_tree, _layout, input.pointer);
        now.hovered = CanFocus(_tree, _layout, hit) ? hit : NodeId{};
        if (moved || input.primaryPressed)
        {
            now.device = InputDevice::Pointer;
        }
        if (_hoverFocuses && moved && now.hovered)
        {
            now.focused = now.hovered;
        }
        if (input.primaryPressed)
        {
            result.pointerUsed = static_cast<bool>(hit);
            now.pressed = now.hovered;
            // A control that takes no focus still keeps the pointer while it is
            // held: a scroll container is dragged, not focused.
            if (!now.pressed && _tree.WidgetOf(hit) != nullptr)
            {
                now.pressed = hit;
            }
            // A click on the game takes focus off the UI, so keys it held go back.
            if (now.hovered || input.grant == InputGrant::Pointer)
            {
                now.focused = now.hovered;
            }
            Dispatch(now.pressed, PointerGesture(WidgetGesture::Press, input.pointer));
        }
        // The press is kept while the pointer wanders, so a slider still follows
        // a cursor that has left it.
        if (input.primaryDown && !input.primaryPressed && moved && now.pressed)
        {
            Dispatch(now.pressed, DragGesture(input.pointer, delta));
        }
        if (input.primaryReleased && now.pressed)
        {
            result.pointerUsed = true;
            Dispatch(now.pressed, PointerGesture(WidgetGesture::Release, input.pointer));
            if (now.pressed == now.hovered)
            {
                now.activated = now.pressed;
            }
            now.pressed = {};
        }
        if (input.wheel.x != 0.f || input.wheel.y != 0.f)
        {
            result.wheelUsed = DispatchWheel(hit, input);
        }
    }

    const Node *focused = _tree.Get(now.focused);
    const bool keys = input.grant == InputGrant::Everything ||
                      (input.grant == InputGrant::Pointer && focused != nullptr && focused->takesKeyboard);
    if (!keys || input.keyboardClaimed)
    {
        _repeating = UiAction::Count;
        return result;
    }
    result.keyboardTaken = true;
    if (std::ranges::any_of(input.actionPressed, [](bool pressed) { return pressed; }))
    {
        now.device = InputDevice::Keys;
    }
    if (input.actionPressed[static_cast<std::size_t>(UiAction::Accept)] && now.focused)
    {
        now.activated = now.focused;
    }
    now.backPressed = input.actionPressed[static_cast<std::size_t>(UiAction::Back)];
    Navigate(input);
    return result;
}

std::array<bool, kUiActionCount> Ui::FiredActions(const UiInput &input)
{
    const auto at = [](UiAction action) { return static_cast<std::size_t>(action); };
    std::array<bool, kUiActionCount> fired = input.actionPressed;

    if (_repeating != UiAction::Count && !input.actionDown[at(_repeating)])
    {
        _repeating = UiAction::Count;
    }
    for (const UiAction action : kMoveActions)
    {
        if (input.actionPressed[at(action)])
        {
            _repeating = action;
            _repeatAt = input.time + kNavRepeatDelaySeconds;
        }
    }
    if (_repeating != UiAction::Count && !input.actionPressed[at(_repeating)] && input.time >= _repeatAt)
    {
        fired[at(_repeating)] = true;
        _repeatAt += kNavRepeatIntervalSeconds;
    }
    return fired;
}

void Ui::Navigate(const UiInput &input)
{
    const std::array<bool, kUiActionCount> fired = FiredActions(input);
    for (const UiAction action : kMoveActions)
    {
        if (!fired[static_cast<std::size_t>(action)])
        {
            continue;
        }
        // The focused control has first claim on a direction: a slider steps on
        // Left and Right rather than handing focus to whatever sits beside it.
        if (Dispatch(_interaction.focused, ActionGesture(action)) == WidgetResponse::Ignored)
        {
            Move(action);
        }
    }
}

void Ui::Move(UiAction action)
{
    NodeId &focused = _interaction.focused;
    if (!focused)
    {
        focused = FirstFocusable(_tree, _layout);
    }
    else
    {
        NodeId next;
        switch (action)
        {
        case UiAction::Next:
            next = NextFocusable(_tree, _layout, focused, TabOrder::Forward);
            break;
        case UiAction::Previous:
            next = NextFocusable(_tree, _layout, focused, TabOrder::Backward);
            break;
        case UiAction::Up:
            next = Neighbour(_tree, _layout, focused, NavDirection::Up, _navWrap);
            break;
        case UiAction::Down:
            next = Neighbour(_tree, _layout, focused, NavDirection::Down, _navWrap);
            break;
        case UiAction::Left:
            next = Neighbour(_tree, _layout, focused, NavDirection::Left, _navWrap);
            break;
        case UiAction::Right:
            next = Neighbour(_tree, _layout, focused, NavDirection::Right, _navWrap);
            break;
        case UiAction::Accept:
        case UiAction::Back:
        case UiAction::Count:
            break;
        }
        if (next)
        {
            focused = next;
        }
    }
    ScrollIntoView(_tree, _layout, focused);
}

void Ui::DrawFocusRing()
{
    const LayoutNode *node = _layout.Get(_interaction.focused);
    const Node *focused = _tree.Get(_interaction.focused);
    if (_interaction.device != InputDevice::Keys || node == nullptr || focused == nullptr)
    {
        return;
    }
    const float scale = _layout.scale;
    const float width = std::max(kMinBorderDevicePixels, std::round(kFocusRingWidth * scale));
    const float gap = std::round(kFocusRingGap * scale);
    const Rect ring{.x = node->rect.x - gap - width,
                    .y = node->rect.y - gap - width,
                    .width = node->rect.width + (2.f * (gap + width)),
                    .height = node->rect.height + (2.f * (gap + width))};
    const float radius = focused->style.cornerRadius > 0.f ? (focused->style.cornerRadius * scale) + gap + width : 0.f;
    _drawList.SetDefaultClip(node->clip);
    _drawList.Quad(ring)
        .Fill({0.f, 0.f, 0.f, 0.f})
        .Border(width, kFocusRingColor)
        .Corners(radius, focused->style.cornerStyle);
    _drawList.SetDefaultClip(kNoClip);
}

void Ui::Sync(Extent viewport)
{
    ASSISI_ASSERT(_nextStep == FrameStep::AwaitingSync, "Ui::Sync called without a ProcessInput before it");
    _nextStep = FrameStep::AwaitingInput;

    _drawList.Clear();
    const float scale = UiScale(viewport, _userScale);
    if (scale > 0.f)
    {
        ComputeLayout(_tree, viewport, scale, _font, _layout);
        DrawTree(_tree, _layout, _drawList, _fontAtlas, _interaction);
        DrawFocusRing();
    }
    _drawList.Finalize();
}

} // namespace Assisi::Mondrian
