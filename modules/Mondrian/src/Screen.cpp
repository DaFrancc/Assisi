/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Screen.hpp>

#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Mondrian/Utf8.hpp>

#include <algorithm>
#include <utility>

namespace Assisi::Mondrian
{
namespace
{

/// The look a text field has until a game restyles it, in UI pixels. A
/// field made with no style at all would be an invisible box, and a player
/// cannot type into what they cannot see.
constexpr float kFieldWidth = 220.f;
constexpr float kFieldTextSize = 28.f;
constexpr float kFieldBorderWidth = 2.f;
constexpr float kFieldCornerRadius = 10.f;
constexpr Padding kFieldPadding{.left = 10.f, .top = 6.f, .right = 10.f, .bottom = 6.f};
constexpr Math::Color4<Math::ColorSpace::Srgb> kFieldColor{0.04f, 0.05f, 0.07f, 1.f};
constexpr Math::Color4<Math::ColorSpace::Srgb> kFieldBorder{0.34f, 0.38f, 0.48f, 1.f};

} // namespace

Style TextFieldStyle()
{
    Style style;
    style.sizing = {Sizing{.min = kFieldWidth, .kind = SizingKind::Grow}, Sizing::Fit()};
    style.padding = kFieldPadding;
    style.textSize = kFieldTextSize;
    style.background = kFieldColor;
    style.borderWidth = kFieldBorderWidth;
    style.borderColor = kFieldBorder;
    style.cornerRadius = kFieldCornerRadius;
    style.cornerStyle = CornerStyle::Rounded;
    return style;
}

Screen::Screen(Ui &ui, ScreenTraits traits, int32_t sortKey, std::string name)
    : _ui(ui), _name(std::move(name)), _sortKey(sortKey), _traits(traits)
{
    // Before anything is built on it, so the nodes it makes can name the
    // built-ins: a widget registry is per tree, and so per screen.
    RegisterBuiltinWidgets(_tree.Widgets());
    _ui.Adopt(*this);
}

Screen::~Screen()
{
    _ui.Forget(*this);
}

void Screen::Show()
{
    _ui.Show(*this);
}

void Screen::Hide()
{
    _ui.Hide(*this);
}

void Screen::OnActivate(NodeId node, std::function<void(Screen &)> act)
{
    // The queue is ignored: this is the half of a screen's behaviour that
    // reaches nothing outside the UI, which is what lets it work in a level
    // that names no systems.
    _tree.SetOnActivate(node, [this, act = std::move(act)](Core::EventQueue &) { act(*this); });
}

bool Screen::Held(NodeId id) const
{
    return _ui.IsHeld(*this, id);
}

const TextLayout *Screen::TextOf(const LayoutNode &placed) const
{
    return placed.text < _layout.texts.size() ? &_layout.texts[placed.text] : nullptr;
}

NodeId Screen::Add(NodeId parent, const Style &style)
{
    const NodeId id = _tree.Create(parent);
    _tree.SetStyle(id, style);
    return id;
}

std::expected<NodeId, NameError> Screen::Add(NodeId parent, const Style &style, std::string_view name)
{
    const std::expected<NodeId, NameError> id = _tree.Create(parent, name);
    if (id)
    {
        _tree.SetStyle(*id, style);
    }
    return id;
}

NodeId Screen::AddText(NodeId parent, const Style &style, std::string_view text)
{
    const NodeId id = Add(parent, style);
    _tree.SetText(id, text);
    return id;
}

std::expected<NodeId, NameError> Screen::AddText(NodeId parent, const Style &style, std::string_view text,
                                                 std::string_view name)
{
    const std::expected<NodeId, NameError> id = Add(parent, style, name);
    if (id)
    {
        _tree.SetText(*id, text);
    }
    return id;
}

ButtonId Screen::AddButton(NodeId parent, std::string_view label)
{
    const NodeId node = _tree.Create(parent);
    _tree.SetText(node, label);
    _tree.SetBehaviour(node, static_cast<uint32_t>(BuiltinWidget::Button));
    _tree.SetFocusable(node, true);
    return {.node = node};
}

std::expected<ButtonId, NameError> Screen::AddButton(NodeId parent, std::string_view label, std::string_view name)
{
    // Checked before the button exists, so a refusal leaves nothing behind.
    if (!name.empty() && Find(name))
    {
        return std::unexpected(NameError::Taken);
    }
    const ButtonId button = AddButton(parent, label);
    if (const std::expected<void, NameError> named = _tree.SetName(button.node, name); !named)
    {
        return std::unexpected(named.error());
    }
    return button;
}

ToggleId Screen::AddToggle(NodeId parent, bool on)
{
    const NodeId node = _tree.Create(parent);
    _tree.SetBehaviour(node, static_cast<uint32_t>(BuiltinWidget::Toggle));
    _tree.SetFocusable(node, true);
    _tree.SetValue(node, on);
    return {.node = node};
}

ContinuousSliderId Screen::AddContinuousSlider(NodeId parent, SliderRange range, float value)
{
    const NodeId node = _tree.Create(parent);
    _tree.SetBehaviour(node, static_cast<uint32_t>(BuiltinWidget::ContinuousSlider));
    _tree.SetFocusable(node, true);
    _tree.SetRange(node, range);
    _tree.SetValue(node, std::clamp(value, range.min, range.max));
    return {.node = node};
}

SteppedSliderId Screen::AddSteppedSlider(NodeId parent, SliderRange range, int32_t steps, int32_t step)
{
    const NodeId node = _tree.Create(parent);
    _tree.SetBehaviour(node, static_cast<uint32_t>(BuiltinWidget::SteppedSlider));
    _tree.SetFocusable(node, true);
    _tree.SetRange(node, range);
    _tree.SetSteps(node, std::max(1, steps));
    _tree.SetValue(node, std::clamp(step, 0, std::max(0, steps - 1)));
    return {.node = node};
}

NodeId Screen::AddScroll(NodeId parent, const Style &style, std::array<bool, kAxisCount> axes)
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

TextFieldId Screen::AddTextField(NodeId parent, TextLines lines)
{
    const NodeId node = _tree.Create(parent);
    _tree.SetStyle(node, TextFieldStyle());
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

std::string_view Screen::GetText(TextFieldId field) const
{
    const Node *node = _tree.Get(field.node);
    return node != nullptr ? std::string_view{node->text} : std::string_view{};
}

void Screen::SetText(TextFieldId field, std::string_view text)
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

void Screen::SetAbility(TextFieldId field, TextAbility ability, bool allowed)
{
    if (Node *node = _tree.Editable(field.node))
    {
        node->edit.abilities[static_cast<std::size_t>(ability)] = allowed;
    }
}

void Screen::SetPlaceholder(TextFieldId field, std::string_view text)
{
    if (Node *node = _tree.Editable(field.node))
    {
        node->edit.placeholder = text;
    }
}

void Screen::SetMask(TextFieldId field, TextMask mask)
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

void Screen::SetMaxLength(TextFieldId field, uint32_t characters)
{
    if (Node *node = _tree.Editable(field.node))
    {
        node->edit.maxLength = characters;
    }
}

void Screen::SetHeight(TextFieldId field, TextHeight height, uint32_t lines)
{
    if (Node *node = _tree.Editable(field.node))
    {
        node->edit.height = height;
        // A bounded field of no lines could never hold anything, which nobody
        // means by it.
        node->edit.lineLimit = std::max(1u, lines);
    }
}

std::expected<void, PatternError> Screen::SetPattern(TextFieldId field, std::string_view pattern, TextCheck check)
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
    SetPattern(field, *std::move(compiled), check);
    return {};
}

void Screen::SetPattern(TextFieldId field, std::shared_ptr<const Pattern> pattern, TextCheck check)
{
    Node *node = _tree.Editable(field.node);
    if (node == nullptr)
    {
        return;
    }
    node->edit.pattern = std::move(pattern);
    node->edit.check = check;
    node->edit.validity = TextValidity::Unchecked;
}

TextValidity Screen::GetValidity(TextFieldId field) const
{
    const Node *node = _tree.Get(field.node);
    return node != nullptr ? node->edit.validity : TextValidity::Unchecked;
}

Rect Screen::GetCaretRect(TextFieldId field) const
{
    const Node *node = _tree.Get(field.node);
    const LayoutNode *placed = _layout.Get(field.node);
    if (node == nullptr || placed == nullptr)
    {
        return {};
    }
    return CaretRect(*node, placed, TextOf(*placed), _layout.scale);
}

void Screen::SetSelectable(NodeId id, bool selectable)
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

void Screen::Step(NodeId slider, int32_t moves)
{
    _ui.Step(*this, slider, moves);
}

void Screen::SetRange(ContinuousSliderId slider, SliderRange range)
{
    _tree.SetRange(slider.node, range);
    SetValue(slider, GetValue(slider));
}

void Screen::SetRange(SteppedSliderId slider, SliderRange range)
{
    _tree.SetRange(slider.node, range);
}

void Screen::SetButtons(ContinuousSliderId slider, SliderButtons buttons)
{
    _tree.SetSliderButtons(slider.node, buttons);
}

void Screen::SetButtons(SteppedSliderId slider, SliderButtons buttons)
{
    _tree.SetSliderButtons(slider.node, buttons);
}

SliderRange Screen::GetRange(ContinuousSliderId slider) const
{
    const Node *node = _tree.Get(slider.node);
    return node != nullptr ? node->range : SliderRange{};
}

SliderRange Screen::GetRange(SteppedSliderId slider) const
{
    const Node *node = _tree.Get(slider.node);
    return node != nullptr ? node->range : SliderRange{};
}

float Screen::GetFraction(ContinuousSliderId slider) const
{
    const SliderRange range = GetRange(slider);
    const float span = range.max - range.min;
    return span > 0.f ? std::clamp((GetValue(slider) - range.min) / span, 0.f, 1.f) : 0.f;
}

float Screen::GetFraction(SteppedSliderId slider) const
{
    const int32_t steps = GetSteps(slider);
    return steps > 1 ? static_cast<float>(GetValue(slider)) / static_cast<float>(steps - 1) : 0.f;
}

int32_t Screen::GetSteps(SteppedSliderId slider) const
{
    const Node *node = _tree.Get(slider.node);
    return node != nullptr ? node->steps : 0;
}

float Screen::GetStepValue(SteppedSliderId slider, int32_t step) const
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

float Screen::GetValue(ContinuousSliderId slider) const
{
    const Node *node = _tree.Get(slider.node);
    return node != nullptr ? Held<float>(node->value) : 0.f;
}

int32_t Screen::GetValue(SteppedSliderId slider) const
{
    const Node *node = _tree.Get(slider.node);
    return node != nullptr ? Held<int32_t>(node->value) : 0;
}

Rect Screen::GetThumbRect(ContinuousSliderId slider) const
{
    const Node *node = _tree.Get(slider.node);
    return node != nullptr ? SliderThumbRect(*node, _layout.Get(slider.node), _layout.scale, GetFraction(slider))
                           : Rect{};
}

Rect Screen::GetThumbRect(SteppedSliderId slider) const
{
    const Node *node = _tree.Get(slider.node);
    return node != nullptr ? SliderThumbRect(*node, _layout.Get(slider.node), _layout.scale, GetFraction(slider))
                           : Rect{};
}

void Screen::SetValue(ContinuousSliderId slider, float value)
{
    const SliderRange range = GetRange(slider);
    if (!Held(slider.node))
    {
        _tree.SetValue(slider.node, std::clamp(value, range.min, range.max));
    }
}

void Screen::SetValue(ToggleId toggle, bool on)
{
    if (!Held(toggle.node))
    {
        _tree.SetValue(toggle.node, on);
    }
}

void Screen::SetValue(SteppedSliderId slider, int32_t step)
{
    const Node *node = _tree.Get(slider.node);
    if (node != nullptr && !Held(slider.node))
    {
        _tree.SetValue(slider.node, std::clamp(step, 0, std::max(0, node->steps - 1)));
    }
}

} // namespace Assisi::Mondrian
