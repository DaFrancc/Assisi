/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Window/InputContext.hpp>

#include <GLFW/glfw3.h>

namespace Assisi::Window
{

InputContext::InputContext(const WindowContext &window) : _window(window.NativeHandle())
{
    /* Snapshot initial mouse position so the first MouseDelta() is (0,0). */
    double xPos = 0.0;
    double yPos = 0.0;
    glfwGetCursorPos(_window, &xPos, &yPos);
    _currMousePos = {static_cast<float>(xPos), static_cast<float>(yPos)};
    _prevMousePos = _currMousePos;

    glfwSetWindowUserPointer(_window, this);
    glfwSetScrollCallback(_window, ScrollCallback);
}

void InputContext::Poll()
{
    _prevKeys = _currKeys;
    for (int k = 0; k < kKeyCount; ++k)
    {
        _currKeys[k] = glfwGetKey(_window, k) == GLFW_PRESS;
    }

    _prevButtons = _currButtons;
    for (int b = 0; b < kButtonCount; ++b)
    {
        _currButtons[b] = glfwGetMouseButton(_window, b) == GLFW_PRESS;
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
    const int idx = static_cast<int>(key);
    return idx >= 0 && idx < kKeyCount && _currKeys[idx];
}

bool InputContext::IsKeyPressed(Key key) const
{
    const int idx = static_cast<int>(key);
    return idx >= 0 && idx < kKeyCount && _currKeys[idx] && !_prevKeys[idx];
}

bool InputContext::IsKeyReleased(Key key) const
{
    const int idx = static_cast<int>(key);
    return idx >= 0 && idx < kKeyCount && !_currKeys[idx] && _prevKeys[idx];
}

bool InputContext::IsMouseButtonDown(MouseButton button) const
{
    const int idx = static_cast<int>(button);
    return idx >= 0 && idx < kButtonCount && _currButtons[idx];
}

bool InputContext::IsMouseButtonPressed(MouseButton button) const
{
    const int idx = static_cast<int>(button);
    return idx >= 0 && idx < kButtonCount && _currButtons[idx] && !_prevButtons[idx];
}

bool InputContext::IsMouseButtonReleased(MouseButton button) const
{
    const int idx = static_cast<int>(button);
    return idx >= 0 && idx < kButtonCount && !_currButtons[idx] && _prevButtons[idx];
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

void InputContext::ScrollCallback(GLFWwindow *window, double /*xoffset*/, double yoffset)
{
    auto *ctx = static_cast<InputContext *>(glfwGetWindowUserPointer(window));
    if (ctx != nullptr)
    {
        ctx->_scrollAccum += static_cast<float>(yoffset);
    }
}

} // namespace Assisi::Window