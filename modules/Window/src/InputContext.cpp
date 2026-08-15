/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Window/InputContext.hpp>

#include <GLFW/glfw3.h>

#include <cstdint>

namespace Assisi::Window
{

InputContext::InputContext(WindowContext &window) : _window(window.NativeHandle())
{
    /* Snapshot initial mouse position so the first MouseDelta() is (0,0). */
    double xPos = 0.0;
    double yPos = 0.0;
    glfwGetCursorPos(_window, &xPos, &yPos);
    _currMousePos = {static_cast<float>(xPos), static_cast<float>(yPos)};
    _prevMousePos = _currMousePos;

    // Subscribe through the window rather than calling glfwSetScrollCallback
    // directly: the window owns the GLFW callbacks and fans out to ImGui (which
    // chains) and us both, instead of one clobbering the other.
    window.OnScroll([this](double /*xOffset*/, double yOffset) { _scrollAccum += static_cast<float>(yOffset); });
}

void InputContext::Poll()
{
    _prevKeys = _currKeys;
    // GLFW key tokens start at Space (32, mirrored by Key::Space); querying
    // codes below that raises GLFW_INVALID_ENUM on every poll. The skipped
    // low entries stay false.
    for (int32_t k = static_cast<int32_t>(Key::Space); k < kKeyCount; ++k)
    {
        _currKeys[static_cast<std::size_t>(k)] = glfwGetKey(_window, k) == GLFW_PRESS;
    }

    _prevButtons = _currButtons;
    for (int32_t b = 0; b < kButtonCount; ++b)
    {
        _currButtons[static_cast<std::size_t>(b)] = glfwGetMouseButton(_window, b) == GLFW_PRESS;
    }

    _prevMousePos = _currMousePos;
    double xPos = 0.0;
    double yPos = 0.0;
    glfwGetCursorPos(_window, &xPos, &yPos);
    _currMousePos = {static_cast<float>(xPos), static_cast<float>(yPos)};
    _mouseDelta = _currMousePos - _prevMousePos;

    _scrollDelta = _scrollAccum;
    _scrollAccum = 0.f;
}

bool InputContext::IsKeyDown(Key key) const
{
    const int32_t idx = static_cast<int32_t>(key);
    return idx >= 0 && idx < kKeyCount && _currKeys[static_cast<std::size_t>(idx)];
}

bool InputContext::IsKeyPressed(Key key) const
{
    const int32_t idx = static_cast<int32_t>(key);
    return idx >= 0 && idx < kKeyCount && _currKeys[static_cast<std::size_t>(idx)] && !_prevKeys[static_cast<std::size_t>(idx)];
}

bool InputContext::IsKeyReleased(Key key) const
{
    const int32_t idx = static_cast<int32_t>(key);
    return idx >= 0 && idx < kKeyCount && !_currKeys[static_cast<std::size_t>(idx)] && _prevKeys[static_cast<std::size_t>(idx)];
}

bool InputContext::IsMouseButtonDown(MouseButton button) const
{
    const int32_t idx = static_cast<int32_t>(button);
    return idx >= 0 && idx < kButtonCount && _currButtons[static_cast<std::size_t>(idx)];
}

bool InputContext::IsMouseButtonPressed(MouseButton button) const
{
    const int32_t idx = static_cast<int32_t>(button);
    return idx >= 0 && idx < kButtonCount && _currButtons[static_cast<std::size_t>(idx)] && !_prevButtons[static_cast<std::size_t>(idx)];
}

bool InputContext::IsMouseButtonReleased(MouseButton button) const
{
    const int32_t idx = static_cast<int32_t>(button);
    return idx >= 0 && idx < kButtonCount && !_currButtons[static_cast<std::size_t>(idx)] && _prevButtons[static_cast<std::size_t>(idx)];
}

glm::vec2 InputContext::MousePosition() const
{
    return _currMousePos;
}

glm::vec2 InputContext::MouseDelta() const
{
    return _mouseDelta;
}

void InputContext::SetMouseCaptured(bool captured)
{
    _mouseCaptured = captured;
    glfwSetInputMode(_window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

bool InputContext::IsMouseCaptured() const
{
    return _mouseCaptured;
}

float InputContext::ScrollDelta() const
{
    return _scrollDelta;
}

} // namespace Assisi::Window