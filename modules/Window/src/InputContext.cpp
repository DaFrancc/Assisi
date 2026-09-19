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
    window.OnScroll([this](double /*xOffset*/, double yOffset) { OnScroll(yOffset); });

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
    _scrollSince = 0.f;
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
    return index ? _keys.taps[*index] : 0;
}

uint32_t InputContext::ClickCount(MouseButton button) const
{
    const std::optional<std::size_t> index = IndexOf(static_cast<int32_t>(button), kButtonCount);
    return index ? _buttons.taps[*index] : 0;
}

void InputContext::OnCursorPosition(double x, double y)
{
    _livePosition = {static_cast<float>(x), static_cast<float>(y)};
}

void InputContext::OnScroll(double yOffset)
{
    _scrollSince += static_cast<float>(yOffset);
}

bool InputContext::IsKeyDown(Key key) const
{
    return IsSet(_keys.down, static_cast<int32_t>(key));
}

bool InputContext::IsKeyPressed(Key key) const
{
    return IsSet(_keys.pressed, static_cast<int32_t>(key));
}

bool InputContext::IsKeyReleased(Key key) const
{
    return IsSet(_keys.released, static_cast<int32_t>(key));
}

bool InputContext::IsMouseButtonDown(MouseButton button) const
{
    return IsSet(_buttons.down, static_cast<int32_t>(button));
}

bool InputContext::IsMouseButtonPressed(MouseButton button) const
{
    return IsSet(_buttons.pressed, static_cast<int32_t>(button));
}

bool InputContext::IsMouseButtonReleased(MouseButton button) const
{
    return IsSet(_buttons.released, static_cast<int32_t>(button));
}

void InputContext::SetMouseCaptured(bool captured)
{
    _mouseCaptured = captured;
    if (_window == nullptr)
    {
        return;
    }

    if (!captured)
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
