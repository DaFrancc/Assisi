/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Window/InputContext.hpp>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace Assisi::Window
{
namespace
{

/// @p value as an index below @p count, or nothing when it is out of range.
std::optional<std::size_t> IndexOf(int32_t value, std::size_t count)
{
    if (value < 0 || static_cast<std::size_t>(value) >= count)
    {
        return std::nullopt;
    }
    return static_cast<std::size_t>(value);
}

bool IsSet(const auto &flags, int32_t value)
{
    const std::optional<std::size_t> index = IndexOf(value, flags.size());
    return index && flags[*index];
}

} // namespace

template <std::size_t Count> bool InputContext::Switches<Count>::Apply(std::size_t index, KeyAction action)
{
    // A press of a key already down, or a release of one already up, is no
    // edge: a repeat is the first, and a duplicate from the platform the other.
    const bool goesDown = action == KeyAction::Press || action == KeyAction::Repeat;
    if (goesDown == live[index])
    {
        return false;
    }
    live[index] = goesDown;
    (goesDown ? pressedSince : releasedSince)[index] = true;
    return goesDown;
}

template <std::size_t Count> void InputContext::Switches<Count>::Tap(std::size_t index, double time, bool continues)
{
    run[index] = continues ? run[index] + 1 : 1;
    lastPress[index] = time;
    tapsSince[index] = run[index];
}

template <std::size_t Count> void InputContext::Switches<Count>::Latch()
{
    // Consumption lasts through the frame an input is released in: one that was
    // up as the frame ending closed is free again from this one.
    for (std::size_t index = 0; index < Count; ++index)
    {
        consumed[index] = consumed[index] && down[index];
    }
    down = live;
    pressed = pressedSince;
    released = releasedSince;
    taps = tapsSince;
    pressedSince.fill(false);
    releasedSince.fill(false);
    tapsSince.fill(0);
}

InputContext::InputContext(WindowContext &window) : _window(window.NativeHandle())
{
    // Subscribe through the window rather than setting GLFW callbacks here: the
    // window owns them and fans out to ImGui (which chains) and us both.
    window.OnKey([this](const KeyEvent &event) { OnKey(event.key, event.action, event.time); });
    window.OnMouseButton([this](const MouseButtonEvent &event)
                         { OnMouseButton(event.button, event.action, event.time); });
    window.OnCursorPosition([this](double x, double y) { OnCursorPosition(x, y); });
    window.OnScroll([this](double xOffset, double yOffset) { OnScroll(xOffset, yOffset); });

    // The cursor reports only when it moves, so start from where it is, and make
    // that the first frame's so its delta is zero.
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(_window, &x, &y);
    OnCursorPosition(x, y);
    _framePosition = _livePosition;
}

void InputContext::Poll()
{
    _keys.Latch();
    _buttons.Latch();
    _frameDelta = _livePosition - _framePosition;
    _framePosition = _livePosition;
    _frameScroll = _scrollSince;
    _scrollSince = {0.f, 0.f};
}

void InputContext::OnKey(Key key, KeyAction action, double time)
{
    const std::optional<std::size_t> index = IndexOf(static_cast<int32_t>(key), kKeyCount);
    if (index && _keys.Apply(*index, action))
    {
        _keys.Tap(*index, time, time - _keys.lastPress[*index] <= _multiTapSeconds);
    }
}

void InputContext::OnMouseButton(MouseButton button, KeyAction action, double time)
{
    const std::optional<std::size_t> index = IndexOf(static_cast<int32_t>(button), kButtonCount);
    if (index && _buttons.Apply(*index, action))
    {
        const bool near = glm::length(_livePosition - _lastClickPosition[*index]) <= kMultiClickSlop;
        _buttons.Tap(*index, time, near && time - _buttons.lastPress[*index] <= _multiTapSeconds);
        _lastClickPosition[*index] = _livePosition;
    }
}

void InputContext::SetMultiTapInterval(double seconds)
{
    if (std::isfinite(seconds))
    {
        _multiTapSeconds = std::clamp(seconds, kMinMultiTapSeconds, kMaxMultiTapSeconds);
    }
}

uint32_t InputContext::TapCount(Key key) const
{
    const std::optional<std::size_t> index = IndexOf(static_cast<int32_t>(key), kKeyCount);
    return index && !_keys.consumed[*index] ? _keys.taps[*index] : 0;
}

uint32_t InputContext::ClickCount(MouseButton button) const
{
    const std::optional<std::size_t> index = IndexOf(static_cast<int32_t>(button), kButtonCount);
    return index && !_buttons.consumed[*index] ? _buttons.taps[*index] : 0;
}

void InputContext::OnCursorPosition(double x, double y)
{
    _livePosition = {static_cast<float>(x), static_cast<float>(y)};
}

void InputContext::OnScroll(double xOffset, double yOffset)
{
    _scrollSince += glm::vec2(static_cast<float>(xOffset), static_cast<float>(yOffset));
}

template <std::size_t Count> void InputContext::Switches<Count>::ConsumeActive()
{
    for (std::size_t index = 0; index < Count; ++index)
    {
        consumed[index] = consumed[index] || down[index] || pressed[index] || released[index];
    }
}

template <std::size_t Count>
bool InputContext::Switches<Count>::Reads(const std::array<bool, Count> &flags, int32_t value,
                                          ConsumedInput counting) const
{
    return IsSet(flags, value) && (counting == ConsumedInput::Include || !IsSet(consumed, value));
}

bool InputContext::IsKeyDown(Key key, ConsumedInput consumed) const
{
    return _keys.Reads(_keys.down, static_cast<int32_t>(key), consumed);
}

bool InputContext::IsKeyPressed(Key key, ConsumedInput consumed) const
{
    return _keys.Reads(_keys.pressed, static_cast<int32_t>(key), consumed);
}

bool InputContext::IsKeyReleased(Key key, ConsumedInput consumed) const
{
    return _keys.Reads(_keys.released, static_cast<int32_t>(key), consumed);
}

bool InputContext::IsMouseButtonDown(MouseButton button, ConsumedInput consumed) const
{
    return _buttons.Reads(_buttons.down, static_cast<int32_t>(button), consumed);
}

bool InputContext::IsMouseButtonPressed(MouseButton button, ConsumedInput consumed) const
{
    return _buttons.Reads(_buttons.pressed, static_cast<int32_t>(button), consumed);
}

bool InputContext::IsMouseButtonReleased(MouseButton button, ConsumedInput consumed) const
{
    return _buttons.Reads(_buttons.released, static_cast<int32_t>(button), consumed);
}

void InputContext::ConsumeKey(Key key)
{
    if (const std::optional<std::size_t> index = IndexOf(static_cast<int32_t>(key), kKeyCount))
    {
        _keys.consumed[*index] = true;
    }
}

void InputContext::ConsumeMouseButton(MouseButton button)
{
    if (const std::optional<std::size_t> index = IndexOf(static_cast<int32_t>(button), kButtonCount))
    {
        _buttons.consumed[*index] = true;
    }
}

void InputContext::ConsumeKeyboard()
{
    _keys.ConsumeActive();
}

void InputContext::ConsumeMouse()
{
    _buttons.ConsumeActive();
    _frameDelta = {0.f, 0.f};
    ConsumeScroll();
}

void InputContext::ConsumeScroll()
{
    _frameScroll = {0.f, 0.f};
}

void InputContext::ConsumeAll()
{
    ConsumeKeyboard();
    ConsumeMouse();
}

void InputContext::SetInputMode(InputMode mode)
{
    _gameMode = mode;
    ApplyCursorMode();
}

InputModeHandle InputContext::PushInputMode(InputMode mode)
{
    const InputModeHandle handle{.id = _nextModeId++};
    _pushedModes.push_back({.id = handle.id, .mode = mode});
    ApplyCursorMode();
    return handle;
}

void InputContext::PopInputMode(InputModeHandle handle)
{
    std::erase_if(_pushedModes, [handle](const PushedMode &pushed) { return pushed.id == handle.id; });
    ApplyCursorMode();
}

InputMode InputContext::GetInputMode() const
{
    return _pushedModes.empty() ? _gameMode : _pushedModes.back().mode;
}

void InputContext::ApplyCursorMode()
{
    if (_window == nullptr)
    {
        return;
    }

    if (!IsMouseCaptured())
    {
        glfwSetInputMode(_window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
        glfwSetInputMode(_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        return;
    }

    glfwSetInputMode(_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    // Giving the window back to the default cursor is what keeps the pointer off
    // screen, not the mode above. A cursor image carries a frame delay, and under
    // Wayland GLFW repaints the window's current cursor on that delay without
    // consulting the cursor mode — so a locked pointer whose window still owns a
    // cursor object has it painted back within a frame or two, stranded wherever
    // the lock froze it. A UI layer that swaps cursor shapes leaves one installed
    // at all times, so there is always one to strand.
    glfwSetCursor(_window, nullptr);

    // Unaccelerated deltas, which is what aiming wants — the pointer acceleration
    // curve a desktop applies is tuned for reaching a target on screen, not for
    // turning. Only meaningful while the cursor is locked, and only where the
    // platform offers it.
    if (glfwRawMouseMotionSupported())
    {
        glfwSetInputMode(_window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }
}

} // namespace Assisi::Window
