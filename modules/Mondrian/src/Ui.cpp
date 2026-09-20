/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Mondrian/Draw.hpp>
#include <Assisi/Mondrian/HitTest.hpp>
#include <Assisi/Mondrian/Style.hpp>
#include <Assisi/Mondrian/Utf8.hpp>

#include <Assisi/Core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
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

/// The editing keys that go on acting while they are held. Erasing does;
/// pasting twenty times because a finger rested on a key is nobody's intention.
constexpr std::array kRepeatingEdits{EditKey::Backspace, EditKey::Delete};

/// How long after a press another one on the same node still continues the run
/// that makes a double click, in seconds.
constexpr double kMultiClickSeconds = 0.4;

/// Which of @p pressed act this frame: every one of them, and the one among
/// @p repeatable still held whose repeat has come round. @p repeat carries
/// which key that is from frame to frame.
template <typename Key, std::size_t Count>
std::array<bool, Count> FiredKeys(const std::array<bool, Count> &pressed, const std::array<bool, Count> &down,
                                  std::span<const Key> repeatable, KeyRepeat<Key> &repeat, double time)
{
    const auto at = [](Key key) { return static_cast<std::size_t>(key); };
    std::array<bool, Count> fired = pressed;

    if (repeat.key != Key::Count && !down[at(repeat.key)])
    {
        repeat.key = Key::Count;
    }
    for (const Key key : repeatable)
    {
        if (pressed[at(key)])
        {
            repeat.key = key;
            repeat.at = time + kNavRepeatDelaySeconds;
        }
    }
    if (repeat.key != Key::Count && !pressed[at(repeat.key)] && time >= repeat.at)
    {
        fired[at(repeat.key)] = true;
        repeat.at += kNavRepeatIntervalSeconds;
    }
    return fired;
}

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

WidgetEvent ActionGesture(UiAction action, TextReach reach, TextStep step)
{
    WidgetEvent event;
    event.gesture = WidgetGesture::Action;
    event.action = action;
    event.reach = reach;
    event.step = step;
    return event;
}

WidgetEvent TypeGesture(std::string_view typed)
{
    WidgetEvent event;
    event.gesture = WidgetGesture::Type;
    event.typed = typed;
    return event;
}

WidgetEvent EditGesture(EditKey key, TextReach reach, TextStep step)
{
    WidgetEvent event;
    event.gesture = WidgetGesture::Edit;
    event.key = key;
    event.reach = reach;
    event.step = step;
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
/// One sample field of many lines for each way of being tall, side by side so
/// that typing into them shows what the three do differently. Their text says
/// which is which and stays short: the row is as tall as its tallest field.
constexpr uint32_t kSampleNoteLines = 3;
constexpr std::string_view kSampleGrowing = "grows forever";
constexpr std::string_view kSampleUpTo = "up to 3 lines";
constexpr std::string_view kSampleExactly = "exactly 3 lines";

constexpr Math::Color4<Math::ColorSpace::Srgb> kFieldColor{0.04f, 0.05f, 0.07f, 1.f};
constexpr float kFieldWidth = 220.f;
constexpr Padding kFieldPadding{.left = 10.f, .top = 6.f, .right = 10.f, .bottom = 6.f};
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

    Style fields;
    fields.sizing = {Sizing::Grow(), Sizing::Fit()};
    fields.gap = kPanelGap;
    fields.childAlign = {Alignment::Start, Alignment::Center};
    const NodeId fieldRow = Add(_tree, panel, "fields", fields);
    const TextFieldId name = AddTextField(fieldRow, TextLines::Single);
    SetText(name, "type here");
    const TextFieldId secret = AddTextField(fieldRow, TextLines::Single);
    SetText(secret, "hunter2");
    SetMask(secret, TextMask::Dots);

    Style notes = fields;
    notes.childAlign = {Alignment::Start, Alignment::Start};
    const NodeId noteRow = Add(_tree, panel, "notes", notes);

    const TextFieldId growing = AddTextField(noteRow, TextLines::Multi);
    SetHeight(growing, TextHeight::Unbounded, 0);
    SetText(growing, kSampleGrowing);

    const TextFieldId upTo = AddTextField(noteRow, TextLines::Multi);
    SetHeight(upTo, TextHeight::UpTo, kSampleNoteLines);
    SetText(upTo, kSampleUpTo);

    const TextFieldId exactly = AddTextField(noteRow, TextLines::Multi);
    SetHeight(exactly, TextHeight::Exactly, kSampleNoteLines);
    SetText(exactly, kSampleExactly);

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

TextFieldId Ui::AddTextField(NodeId parent, TextLines lines)
{
    Style style;
    style.sizing = {Sizing{.min = kFieldWidth, .kind = SizingKind::Grow}, Sizing::Fit()};
    style.padding = kFieldPadding;
    style.textSize = kButtonSize;
    style.background = kFieldColor;
    style.borderWidth = kPanelBorderWidth;
    style.borderColor = kPanelBorder;
    style.cornerRadius = kButtonRadius;
    style.cornerStyle = CornerStyle::Rounded;

    const NodeId node = _tree.Create(parent);
    _tree.SetStyle(node, style);
    _tree.SetBehaviour(node, static_cast<uint32_t>(BuiltinWidget::TextField));
    _tree.SetFocusable(node, true);
    // A focused field has the keyboard even where the game otherwise has it:
    // typing into a box must not also drive the player's character.
    _tree.SetTakesKeyboard(node, true);

    TextEdit edit;
    edit.editing = TextEditing::Editable;
    edit.lines = lines;
    _tree.SetTextEdit(node, edit);
    return {.node = node};
}

std::string_view Ui::GetText(TextFieldId field) const
{
    const Node *node = _tree.Get(field.node);
    return node != nullptr ? std::string_view{node->text} : std::string_view{};
}

void Ui::SetText(TextFieldId field, std::string_view text)
{
    Node *node = _tree.Editable(field.node);
    if (node == nullptr)
    {
        return;
    }
    _tree.SetText(field.node, text);
    node->edit.caret = CharacterCount(node->text);
    node->edit.anchor = node->edit.caret;
    // Text put in from code is finished text, so a field with a pattern says
    // what it makes of it rather than reporting what the last text was worth.
    CommitText(*node);
}

void Ui::SetAbility(TextFieldId field, TextAbility ability, bool allowed)
{
    if (Node *node = _tree.Editable(field.node))
    {
        node->edit.abilities[static_cast<std::size_t>(ability)] = allowed;
    }
}

void Ui::SetMask(TextFieldId field, TextMask mask)
{
    Node *node = _tree.Editable(field.node);
    if (node == nullptr)
    {
        return;
    }
    node->edit.mask = mask;
    // Hiding the text and then letting it be copied out defeats the hiding.
    // Turned off rather than forbidden: a field masked only against someone
    // reading over a shoulder can have them back.
    if (mask == TextMask::Dots)
    {
        SetAbility(field, TextAbility::Copy, false);
        SetAbility(field, TextAbility::Cut, false);
    }
}

void Ui::SetMaxLength(TextFieldId field, uint32_t characters)
{
    if (Node *node = _tree.Editable(field.node))
    {
        node->edit.maxLength = characters;
    }
}

void Ui::SetHeight(TextFieldId field, TextHeight height, uint32_t lines)
{
    if (Node *node = _tree.Editable(field.node))
    {
        node->edit.height = height;
        // A bounded field of no lines could never hold anything, which nobody
        // means by it.
        node->edit.lineLimit = std::max(1u, lines);
    }
}

std::expected<void, PatternError> Ui::SetPattern(TextFieldId field, std::string_view pattern, TextCheck check)
{
    Node *node = _tree.Editable(field.node);
    if (node == nullptr)
    {
        return {};
    }
    if (pattern.empty())
    {
        node->edit.pattern.reset();
        node->edit.validity = TextValidity::Unchecked;
        return {};
    }

    std::expected<std::shared_ptr<const Pattern>, PatternError> compiled = CompilePattern(pattern);
    if (!compiled)
    {
        return std::unexpected(std::move(compiled.error()));
    }
    node->edit.pattern = *std::move(compiled);
    node->edit.check = check;
    node->edit.validity = TextValidity::Unchecked;
    return {};
}

TextValidity Ui::GetValidity(TextFieldId field) const
{
    const Node *node = _tree.Get(field.node);
    return node != nullptr ? node->edit.validity : TextValidity::Unchecked;
}

Rect Ui::GetCaretRect(TextFieldId field) const
{
    const Node *node = _tree.Get(field.node);
    const LayoutNode *placed = _layout.Get(field.node);
    if (node == nullptr || placed == nullptr)
    {
        return {};
    }
    return CaretRect(*node, placed, TextOf(*placed), _layout.scale);
}

void Ui::SetSelectable(NodeId id, bool selectable)
{
    Node *node = _tree.Editable(id);
    if (node == nullptr)
    {
        return;
    }
    node->edit.editing = selectable ? TextEditing::Selectable : TextEditing::None;
    _tree.SetBehaviour(id, selectable ? static_cast<uint32_t>(BuiltinWidget::TextField) : 0);
    // Copying needs somewhere for Ctrl+C to land, and that is focus.
    _tree.SetFocusable(id, selectable);
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

    // Leaving a field is as much a way of finishing with it as pressing Enter,
    // so a field judged on commit is judged when focus goes elsewhere. Here
    // rather than wherever focus moves, because it moves from several places.
    if (_interaction.focused != _focusedLast)
    {
        if (Node *left = _tree.Editable(_focusedLast))
        {
            CommitText(*left);
        }
        _focusedLast = _interaction.focused;
    }

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
        _submitted.clear();
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
            node->onChange(*_events, *node);
        }
    }
    _changed.clear();
    // After the changes, so a field that was edited and then finished in one
    // frame announces what it holds before it announces that it is done.
    for (const NodeId id : _submitted)
    {
        if (const Node *node = _tree.Get(id); node != nullptr && node->onSubmit)
        {
            node->onSubmit(*_events, *node);
        }
    }
    _submitted.clear();
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

const TextLayout *Ui::TextOf(const LayoutNode &placed) const
{
    return placed.text < _layout.texts.size() ? &_layout.texts[placed.text] : nullptr;
}

WidgetView Ui::ViewOf(NodeId id, const WidgetType &widget) const
{
    const LayoutNode *placed = _layout.Get(id);
    WidgetView view;
    view.node = _tree.Get(id);
    view.layout = placed;
    view.text = placed != nullptr ? TextOf(*placed) : nullptr;
    view.clipboard = &_clipboard;
    view.context = widget.context;
    view.scale = _layout.scale;
    view.id = id;
    view.focused = _interaction.focused == id;
    view.pressed = _interaction.pressed == id;
    view.hovered = _interaction.hovered == id;
    return view;
}

Core::CursorShape Ui::CursorOver(NodeId id, Point pointer) const
{
    const WidgetType *widget = _tree.WidgetOf(id);
    if (widget == nullptr || widget->cursor == nullptr || _layout.Get(id) == nullptr)
    {
        return Core::CursorShape::Arrow;
    }
    return widget->cursor(ViewOf(id, *widget), pointer);
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

    const WidgetResponse response = widget->input(ViewOf(id, *widget), *node, event);
    if (response == WidgetResponse::Changed && std::ranges::find(_changed, id) == _changed.end())
    {
        _changed.push_back(id);
    }
    if (response == WidgetResponse::Submitted && std::ranges::find(_submitted, id) == _submitted.end())
    {
        _submitted.push_back(id);
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
            WidgetEvent press = PointerGesture(WidgetGesture::Press, input.pointer);
            press.clicks = CountClicks(now.pressed, input.time);
            Dispatch(now.pressed, press);
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

        // What has hold of the pointer decides its shape, so a field dragging
        // a selection goes on saying so even where the pointer has wandered
        // off the box.
        result.cursor = CursorOver(now.pressed ? now.pressed : hit, input.pointer);
    }

    const Node *focused = _tree.Get(now.focused);
    const bool keys = input.grant == InputGrant::Everything ||
                      (input.grant == InputGrant::Pointer && focused != nullptr && focused->takesKeyboard);
    if (!keys || input.keyboardClaimed)
    {
        _repeatAction.key = UiAction::Count;
        _repeatEdit.key = EditKey::Count;
        return result;
    }
    result.keyboardTaken = true;
    if (std::ranges::any_of(input.actionPressed, [](bool pressed) { return pressed; }))
    {
        now.device = InputDevice::Keys;
    }
    // Accept and Back go to the focused control before they mean anything
    // general, on the same footing as a direction: Enter in a field is a
    // newline or a submission, not the click a button would read it as.
    if (input.actionPressed[static_cast<std::size_t>(UiAction::Accept)] && now.focused &&
        Dispatch(now.focused, ActionGesture(UiAction::Accept, input.reach, input.step)) == WidgetResponse::Ignored)
    {
        now.activated = now.focused;
    }
    if (input.actionPressed[static_cast<std::size_t>(UiAction::Back)])
    {
        now.backPressed =
            Dispatch(now.focused, ActionGesture(UiAction::Back, input.reach, input.step)) == WidgetResponse::Ignored;
    }
    Navigate(input);
    Write(input);
    return result;
}

std::array<bool, kUiActionCount> Ui::FiredActions(const UiInput &input)
{
    return FiredKeys(input.actionPressed, input.actionDown, std::span<const UiAction>{kMoveActions}, _repeatAction,
                     input.time);
}

std::array<bool, kEditKeyCount> Ui::FiredEdits(const UiInput &input)
{
    return FiredKeys(input.editPressed, input.editDown, std::span<const EditKey>{kRepeatingEdits}, _repeatEdit,
                     input.time);
}

void Ui::Write(const UiInput &input)
{
    if (!input.typed.empty())
    {
        Dispatch(_interaction.focused, TypeGesture(input.typed));
    }

    const std::array<bool, kEditKeyCount> fired = FiredEdits(input);
    for (std::size_t index = 0; index < kEditKeyCount; ++index)
    {
        if (fired[index])
        {
            Dispatch(_interaction.focused, EditGesture(static_cast<EditKey>(index), input.reach, input.step));
        }
    }
}

uint32_t Ui::CountClicks(NodeId node, double time)
{
    const bool continues = node == _lastPressed && time - _lastPressAt <= kMultiClickSeconds;
    _clicks = continues ? _clicks + 1 : 1;
    _lastPressed = node;
    _lastPressAt = time;
    return _clicks;
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
        // Left and Right rather than handing focus to whatever sits beside it,
        // and a field moves its caret.
        if (Dispatch(_interaction.focused, ActionGesture(action, input.reach, input.step)) == WidgetResponse::Ignored)
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
