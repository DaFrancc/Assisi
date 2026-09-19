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
    tree.SetFocusable(AddText(tree, row, label, style, label), true);
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

Ui::Ui() : _picture(BuildSampleScreen(_tree))
{
}

void Ui::SetPlaceholderTexture(TextureId texture)
{
    _tree.SetImage(_picture, texture, kWholeTexture);
}

InputResult Ui::ProcessInput(const UiInput &input)
{
    ASSISI_ASSERT(_nextStep == FrameStep::AwaitingInput, "Ui::ProcessInput called twice without a Sync between");
    _nextStep = FrameStep::AwaitingSync;

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

    const bool moved = input.pointer.x != _lastPointer.x || input.pointer.y != _lastPointer.y;
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
            // A click on the game takes focus off the UI, so keys it held go back.
            if (now.hovered || input.grant == InputGrant::Pointer)
            {
                now.focused = now.hovered;
            }
        }
        if (input.primaryReleased && now.pressed)
        {
            result.pointerUsed = true;
            if (now.pressed == now.hovered)
            {
                now.activated = now.pressed;
            }
            now.pressed = {};
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

void Ui::Navigate(const UiInput &input)
{
    constexpr std::array kMoves{UiAction::Up,    UiAction::Down, UiAction::Left,
                                UiAction::Right, UiAction::Next, UiAction::Previous};
    const auto at = [](UiAction action) { return static_cast<std::size_t>(action); };

    if (_repeating != UiAction::Count && !input.actionDown[at(_repeating)])
    {
        _repeating = UiAction::Count;
    }
    for (const UiAction action : kMoves)
    {
        if (input.actionPressed[at(action)])
        {
            Move(action);
            _repeating = action;
            _repeatAt = input.time + kNavRepeatDelaySeconds;
        }
    }
    if (_repeating != UiAction::Count && !input.actionPressed[at(_repeating)] && input.time >= _repeatAt)
    {
        Move(_repeating);
        _repeatAt += kNavRepeatIntervalSeconds;
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
        DrawTree(_tree, _layout, _drawList, _fontAtlas);
        DrawFocusRing();
    }
    _drawList.Finalize();
}

} // namespace Assisi::Mondrian
