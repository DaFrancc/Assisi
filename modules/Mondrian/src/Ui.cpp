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
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

namespace Assisi::Mondrian
{
namespace
{

/// The ring around the focused node while keys were used last: its thickness,
/// and the space between it and the node, in UI pixels.
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

} // namespace

Ui::Ui(Core::EventQueue &events) : _events(events)
{
}
Ui::~Ui() = default;

// -----------------------------------------------------------------------------
// Screens
// -----------------------------------------------------------------------------

void Ui::Adopt(Screen &screen)
{
    _screens.push_back(&screen);
}

void Ui::Forget(Screen &screen)
{
    ASSISI_ASSERT(!_announcing, "a screen was destroyed from a callback the UI was announcing it through");

    const std::vector<Screen *>::iterator at = std::ranges::find(_screens, &screen);
    if (at == _screens.end())
    {
        return;
    }
    // Hidden before it goes, so the session naming its nodes is handed on while
    // the tree those ids name is still there.
    screen._shown = false;
    if (_inputScreen == &screen)
    {
        _inputScreen = nullptr;
        _session = InputSession{};
        _changed.clear();
        _submitted.clear();
    }
    _screens.erase(at);
    RefreshInputScreen();
}

bool Ui::Above(const Screen &screen, const Screen &other)
{
    return screen._sortKey != other._sortKey ? screen._sortKey > other._sortKey : screen._shownAt > other._shownAt;
}

void Ui::Show(Screen &screen)
{
    if (!screen._shown)
    {
        screen._shown = true;
        screen._shownAt = ++_showSequence;
    }
    RefreshInputScreen();
}

void Ui::Hide(Screen &screen)
{
    if (!screen._shown)
    {
        return;
    }
    screen._shown = false;
    RefreshInputScreen();
}

bool Ui::Back()
{
    Screen *top = _inputScreen;
    // A screen that locks the keys keeps them: only code hides it, so Back is
    // not a way out of one.
    if (top == nullptr || top->Traits().input != ScreenInput::ConsumeInput)
    {
        return false;
    }
    Hide(*top);
    return true;
}

Screen *Ui::TopInputScreen()
{
    Screen *top = nullptr;
    for (Screen *const screen : _screens)
    {
        if (!screen->_shown || screen->Traits().input == ScreenInput::NoConsume)
        {
            continue;
        }
        if (top == nullptr || Above(*screen, *top))
        {
            top = screen;
        }
    }
    return top;
}

void Ui::RefreshInputScreen()
{
    Screen *next = TopInputScreen();
    if (next == _inputScreen)
    {
        return;
    }

    if (_inputScreen != nullptr)
    {
        // Leaving a field is as much a way of finishing with it as pressing
        // Enter. Against the old screen's tree, and before the session goes:
        // afterwards the id names a slot on a different screen.
        if (Node *leaving = _inputScreen->Tree().Editable(_session.interaction.focused))
        {
            CommitText(*leaving);
        }
        _inputScreen->_focused = _session.interaction.focused;
    }

    _session = InputSession{};
    _changed.clear();
    _submitted.clear();
    _inputScreen = next;
    if (_inputScreen != nullptr)
    {
        _session.interaction.focused = _inputScreen->_focused;
        _session.focusedLast = _session.interaction.focused;
    }
}

std::vector<Screen *> Ui::DrawOrder()
{
    std::vector<Screen *> order;
    for (Screen *const screen : _screens)
    {
        if (screen->_shown)
        {
            order.push_back(screen);
        }
    }
    std::ranges::sort(order, [](const Screen *a, const Screen *b) { return Above(*b, *a); });

    const std::vector<Screen *>::reverse_iterator hider =
        std::ranges::find_if(order.rbegin(), order.rend(), [](const Screen *screen)
                             { return screen->Traits().beneath == ScreenBeneath::HidesBeneath; });
    if (hider != order.rend())
    {
        order.erase(order.begin(), hider.base() - 1);
    }
    return order;
}

bool Ui::TakesInput() const
{
    return _inputScreen != nullptr;
}

bool Ui::PausesWorld() const
{
    return std::ranges::any_of(_screens, [](const Screen *screen)
                               { return screen->_shown && screen->Traits().pause == ScreenPause::Pause; });
}

void Ui::SetFocus(Screen &screen, NodeId id)
{
    if (_inputScreen == &screen)
    {
        _session.interaction.focused = id;
        return;
    }
    screen._focused = id;
}

bool Ui::IsHeld(const Screen &screen, NodeId id) const
{
    return _inputScreen == &screen && _session.interaction.pressed == id;
}

// -----------------------------------------------------------------------------
// The frame
// -----------------------------------------------------------------------------

InputResult Ui::ProcessInput(const UiInput &input)
{
    ASSISI_ASSERT(_nextStep == FrameStep::AwaitingInput, "Ui::ProcessInput called twice without a Sync between");
    _nextStep = FrameStep::AwaitingSync;

    // The screen the frame belongs to, taken once: Back can change which screen
    // has the keys partway through, and everything below is about this one.
    Screen *const screen = _inputScreen;
    InputResult result;
    if (screen != nullptr)
    {
        result = Interact(*screen, input);

        // Leaving a field is as much a way of finishing with it as pressing
        // Enter, so a field judged on commit is judged when focus goes
        // elsewhere. Here rather than wherever focus moves, because it moves
        // from several places.
        if (_session.interaction.focused != _session.focusedLast)
        {
            if (Node *left = screen->Tree().Editable(_session.focusedLast))
            {
                CommitText(*left);
            }
            _session.focusedLast = _session.interaction.focused;
        }
    }
    else
    {
        _session.interaction.hovered = {};
        _session.interaction.pressed = {};
        _session.interaction.activated = {};
    }

    AdvanceScrolling(std::max(0.0, input.time - _lastTime));
    _lastTime = input.time;
    if (screen != nullptr && _session.interaction.activated)
    {
        Dispatch(*screen, _session.interaction.activated, PointerGesture(WidgetGesture::Activate, _lastPointer));
    }
    Announce(screen);

    // After the announcing: a screen says what its controls did before it goes,
    // and the session that names its nodes is still the one those ids belong to.
    if (_backFired)
    {
        _backFired = false;
        Back();
    }
    return result;
}

void Ui::Announce(Screen *screen)
{
    if (screen == nullptr)
    {
        _changed.clear();
        _submitted.clear();
        return;
    }
    // Taken before any of them runs, because a callback may hide or pop its own
    // screen and that clears these very lists. Walking the members instead
    // would be walking a vector something else is emptying.
    const NodeId activatedId = _session.interaction.activated;
    std::vector<NodeId> changed = std::exchange(_changed, {});
    const std::vector<NodeId> submitted = std::exchange(_submitted, {});

    // Destroying a screen from here is the one thing that cannot be made safe:
    // the tree being announced from would go with it.
    _announcing = true;

    const NodeTree &tree = screen->Tree();
    if (const Node *activated = tree.Get(activatedId); activated != nullptr && activated->onActivate)
    {
        activated->onActivate(_events);
    }
    // What the activation moved is this frame's change too: a button that
    // steps a slider is heard in the frame it was pressed in, as the key it
    // stands for would be. Taken again rather than taken once, later, because
    // a callback that hid this screen has already emptied the list.
    for (const NodeId id : std::exchange(_changed, {}))
    {
        if (std::ranges::find(changed, id) == changed.end())
        {
            changed.push_back(id);
        }
    }
    for (const NodeId id : changed)
    {
        if (const Node *node = tree.Get(id); node != nullptr && node->onChange)
        {
            node->onChange(_events, *node);
        }
    }
    // After the changes, so a field that was edited and then finished in one
    // frame announces what it holds before it announces that it is done.
    for (const NodeId id : submitted)
    {
        if (const Node *node = tree.Get(id); node != nullptr && node->onSubmit)
        {
            node->onSubmit(_events, *node);
        }
    }
    _announcing = false;
}

void Ui::AdvanceScrolling(double seconds)
{
    // Within half a UI pixel is arrived: the rest would creep for frames
    // nobody can see, and layout snaps to whole pixels anyway.
    constexpr float kSettled = 0.5f;

    for (Screen *const screen : _screens)
    {
        if (!screen->_shown)
        {
            continue;
        }
        NodeTree &tree = screen->Tree();
        for (uint32_t index = 0; index < tree.Slots().size(); ++index)
        {
            Node *node = tree.Editable(tree.IdOf(index));
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
}

bool Ui::Editing(const Screen &screen) const
{
    const Node *focused = screen.Tree().Get(_session.interaction.focused);
    return focused != nullptr && focused->takesKeyboard;
}

WidgetView Ui::ViewOf(const Screen &screen, NodeId id, const WidgetType &widget) const
{
    const LayoutNode *placed = screen._layout.Get(id);
    WidgetView view;
    view.node = screen.Tree().Get(id);
    view.layout = placed;
    view.text = placed != nullptr ? screen.TextOf(*placed) : nullptr;
    view.clipboard = &_clipboard;
    view.context = widget.context;
    view.scale = screen._layout.scale;
    view.id = id;
    view.focused = _session.interaction.focused == id;
    view.pressed = _session.interaction.pressed == id;
    view.hovered = _session.interaction.hovered == id;
    return view;
}

Core::CursorShape Ui::CursorOver(const Screen &screen, NodeId id, Point pointer) const
{
    const WidgetType *widget = screen.Tree().WidgetOf(id);
    if (widget == nullptr || widget->cursor == nullptr || screen._layout.Get(id) == nullptr)
    {
        return Core::CursorShape::Arrow;
    }
    return widget->cursor(ViewOf(screen, id, *widget), pointer);
}

WidgetResponse Ui::Dispatch(Screen &screen, NodeId id, const WidgetEvent &event)
{
    const WidgetType *widget = screen.Tree().WidgetOf(id);
    const LayoutNode *placed = screen._layout.Get(id);
    Node *node = screen.Tree().Editable(id);
    if (widget == nullptr || widget->input == nullptr || placed == nullptr || node == nullptr)
    {
        return WidgetResponse::Ignored;
    }

    const WidgetResponse response = widget->input(ViewOf(screen, id, *widget), *node, event);
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

void Ui::Step(Screen &screen, NodeId target, int32_t moves)
{
    // Only the screen with the keys records changes, since a node id means
    // nothing on another; and a key press could not reach a slider anywhere
    // else either.
    const Node *node = screen.Tree().Get(target);
    if (&screen != _inputScreen || node == nullptr || !node->enabled)
    {
        return;
    }

    WidgetEvent press;
    press.gesture = WidgetGesture::Action;
    press.action = moves > 0 ? UiAction::Right : UiAction::Left;
    // Widened first: the magnitude of INT32_MIN does not fit an int32_t.
    const int64_t count = std::abs(static_cast<int64_t>(moves));
    for (int64_t move = 0; move < count; ++move)
    {
        // A hidden slider has no layout, so Dispatch hands it nothing. A move
        // that changed nothing was at the slider's end, and every one after it
        // would be too, so this is bounded by the slider's range and not by
        // however large a count the file wrote.
        if (Dispatch(screen, target, press) != WidgetResponse::Changed)
        {
            return;
        }
    }
}

bool Ui::DispatchWheel(Screen &screen, NodeId hit, const UiInput &input)
{
    const WidgetEvent event = WheelGesture(input.pointer, input.wheel);
    // Up the tree from whatever the pointer is over: a wheel over a button
    // inside a list scrolls the list, as it does everywhere else.
    for (NodeId id = hit; id; id = screen.Tree().Get(id)->parent)
    {
        if (Dispatch(screen, id, event) != WidgetResponse::Ignored)
        {
            return true;
        }
    }
    return false;
}

InputResult Ui::Interact(Screen &screen, const UiInput &input)
{
    InputResult result;
    Interaction &now = _session.interaction;
    NodeTree &tree = screen.Tree();
    const LayoutResult &layout = screen._layout;
    now.activated = {};

    // A focused node that has gone, hidden or been disabled hands focus on, so
    // the keys still have somewhere to act. Not before the first layout, which
    // has placed nothing yet: focus set ahead of it would be lost.
    const bool laidOut = layout.scale > 0.f;
    if (laidOut && now.focused && !CanFocus(tree, layout, now.focused))
    {
        now.focused = FirstFocusable(tree, layout);
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
        const NodeId hit = HitTest(tree, layout, input.pointer);
        now.hovered = CanFocus(tree, layout, hit) ? hit : NodeId{};
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
            if (!now.pressed && tree.WidgetOf(hit) != nullptr)
            {
                now.pressed = hit;
            }
            // A click on the game takes focus off the UI, so keys it held go back.
            if (now.hovered || input.grant == InputGrant::Pointer)
            {
                now.focused = now.hovered;
            }
            else if (Editing(screen))
            {
                // A press anywhere but on the field being typed into ends the
                // typing. A button keeps its focus through the same press,
                // because focus on a button is only a mark of where the keys
                // are, not a claim on them.
                now.focused = {};
            }
            WidgetEvent press = PointerGesture(WidgetGesture::Press, input.pointer);
            press.clicks = CountClicks(now.pressed, input.time);
            Dispatch(screen, now.pressed, press);
        }
        // The press is kept while the pointer wanders, so a slider still follows
        // a cursor that has left it.
        if (input.primaryDown && !input.primaryPressed && moved && now.pressed)
        {
            Dispatch(screen, now.pressed, DragGesture(input.pointer, delta));
        }
        if (input.primaryReleased && now.pressed)
        {
            result.pointerUsed = true;
            Dispatch(screen, now.pressed, PointerGesture(WidgetGesture::Release, input.pointer));
            if (now.pressed == now.hovered)
            {
                now.activated = now.pressed;
            }
            now.pressed = {};
        }
        if (input.wheel.x != 0.f || input.wheel.y != 0.f)
        {
            result.wheelUsed = DispatchWheel(screen, hit, input);
        }

        // What has hold of the pointer decides its shape, so a field dragging
        // a selection goes on saying so even where the pointer has wandered
        // off the box.
        result.cursor = CursorOver(screen, now.pressed ? now.pressed : hit, input.pointer);
    }

    const Node *focused = tree.Get(now.focused);
    const bool keys = input.grant == InputGrant::Everything ||
                      (input.grant == InputGrant::Pointer && focused != nullptr && focused->takesKeyboard);
    if (!keys || input.keyboardClaimed)
    {
        _session.repeatAction.key = UiAction::Count;
        _session.repeatEdit.key = EditKey::Count;
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
        Dispatch(screen, now.focused, ActionGesture(UiAction::Accept, input.reach, input.step)) ==
            WidgetResponse::Ignored)
    {
        now.activated = now.focused;
    }
    if (input.actionPressed[static_cast<std::size_t>(UiAction::Back)])
    {
        const bool editing = Editing(screen);
        if (Dispatch(screen, now.focused, ActionGesture(UiAction::Back, input.reach, input.step)) ==
            WidgetResponse::Ignored)
        {
            // Back gets out of the field first and closes the screen second, so
            // that leaving a box does not also leave the menu it is on.
            _backFired = !editing;
            if (editing)
            {
                now.focused = {};
            }
        }
    }
    Navigate(screen, input);
    Write(screen, input);
    return result;
}

std::array<bool, kUiActionCount> Ui::FiredActions(const UiInput &input)
{
    return FiredKeys(input.actionPressed, input.actionDown, std::span<const UiAction>{kMoveActions},
                     _session.repeatAction, input.time);
}

std::array<bool, kEditKeyCount> Ui::FiredEdits(const UiInput &input)
{
    return FiredKeys(input.editPressed, input.editDown, std::span<const EditKey>{kRepeatingEdits}, _session.repeatEdit,
                     input.time);
}

void Ui::Write(Screen &screen, const UiInput &input)
{
    if (!input.typed.empty())
    {
        Dispatch(screen, _session.interaction.focused, TypeGesture(input.typed));
    }

    const std::array<bool, kEditKeyCount> fired = FiredEdits(input);
    for (std::size_t index = 0; index < kEditKeyCount; ++index)
    {
        if (fired[index])
        {
            Dispatch(screen, _session.interaction.focused,
                     EditGesture(static_cast<EditKey>(index), input.reach, input.step));
        }
    }
}

uint32_t Ui::CountClicks(NodeId node, double time)
{
    const bool continues = node == _session.lastPressed && time - _session.lastPressAt <= kMultiClickSeconds;
    _session.clicks = continues ? _session.clicks + 1 : 1;
    _session.lastPressed = node;
    _session.lastPressAt = time;
    return _session.clicks;
}

void Ui::Navigate(Screen &screen, const UiInput &input)
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
        if (Dispatch(screen, _session.interaction.focused, ActionGesture(action, input.reach, input.step)) ==
            WidgetResponse::Ignored)
        {
            Move(screen, action);
        }
    }
}

void Ui::Move(Screen &screen, UiAction action)
{
    NodeId &focused = _session.interaction.focused;
    NodeTree &tree = screen.Tree();
    const LayoutResult &layout = screen._layout;
    if (!focused)
    {
        focused = FirstFocusable(tree, layout);
    }
    else
    {
        NodeId next;
        switch (action)
        {
        case UiAction::Next:
            next = NextFocusable(tree, layout, focused, TabOrder::Forward);
            break;
        case UiAction::Previous:
            next = NextFocusable(tree, layout, focused, TabOrder::Backward);
            break;
        case UiAction::Up:
            next = Neighbour(tree, layout, focused, NavDirection::Up, _navWrap);
            break;
        case UiAction::Down:
            next = Neighbour(tree, layout, focused, NavDirection::Down, _navWrap);
            break;
        case UiAction::Left:
            next = Neighbour(tree, layout, focused, NavDirection::Left, _navWrap);
            break;
        case UiAction::Right:
            next = Neighbour(tree, layout, focused, NavDirection::Right, _navWrap);
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
    ScrollIntoView(tree, layout, focused);
}

void Ui::DrawFocusRing(const Screen &screen)
{
    const LayoutNode *node = screen._layout.Get(_session.interaction.focused);
    const Node *focused = screen.Tree().Get(_session.interaction.focused);
    if (_session.interaction.device != InputDevice::Keys || node == nullptr || focused == nullptr)
    {
        return;
    }
    const float scale = screen._layout.scale;
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
    const float scale = UiScale(viewport, _userScale, _scaleMatch);
    if (scale > 0.f)
    {
        for (Screen *screen : DrawOrder())
        {
            ComputeLayout(screen->Tree(), viewport, scale, _font, screen->_layout);
            // Only the screen with the keys draws anything as hovered, pressed
            // or focused. A node id means nothing on another screen, so handing
            // this one's ids to a second tree would mark whatever shares a slot.
            if (screen != _inputScreen)
            {
                DrawTree(screen->Tree(), screen->_layout, _drawList, _fontAtlas, Interaction{});
                continue;
            }
            DrawTree(screen->Tree(), screen->_layout, _drawList, _fontAtlas, _session.interaction);
            DrawFocusRing(*screen);
        }
    }
    _drawList.Finalize();
}

} // namespace Assisi::Mondrian
