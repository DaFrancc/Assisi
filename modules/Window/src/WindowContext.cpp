/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <GLFW/glfw3.h>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Window/WindowContext.hpp>

namespace Assisi::Window
{
WindowContext::WindowContext(const WindowConfiguration &configuration) : _glfwLibrary(GlfwLibrary::Acquire())
{
    if (!_glfwLibrary || !_glfwLibrary->IsValid())
    {
        return;
    }

    /* Vulkan owns presentation — GLFW must not create a client API context. */
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_DECORATED, configuration.Undecorated ? GLFW_FALSE : GLFW_TRUE);

    _nativeWindowHandle =
        glfwCreateWindow(configuration.Width, configuration.Height, configuration.Title, nullptr, nullptr);

    if (_nativeWindowHandle == nullptr)
    {
        Assisi::Core::Log::Error("Failed to create GLFW window.");
        return;
    }

    InstallCallbacks();

    _isValid = true;
}

void WindowContext::InstallCallbacks()
{
    // WindowContext owns the user pointer; the ImGui GLFW backend deliberately
    // does not use it (it keys off the ImGui context), so this is safe. ImGui,
    // initialised later with install_callbacks=true, chains to the callbacks
    // installed here rather than replacing them.
    glfwSetWindowUserPointer(_nativeWindowHandle, this);
    glfwSetFramebufferSizeCallback(_nativeWindowHandle, FramebufferSizeTrampoline);
    glfwSetScrollCallback(_nativeWindowHandle, ScrollTrampoline);
    glfwSetWindowRefreshCallback(_nativeWindowHandle, WindowRefreshTrampoline);
}

void WindowContext::FramebufferSizeTrampoline(GLFWwindow *window, int width, int height)
{
    if (auto *self = static_cast<WindowContext *>(glfwGetWindowUserPointer(window)))
    {
        for (const auto &callback : self->_framebufferSizeCallbacks)
            callback(width, height);
    }
}

void WindowContext::ScrollTrampoline(GLFWwindow *window, double xOffset, double yOffset)
{
    if (auto *self = static_cast<WindowContext *>(glfwGetWindowUserPointer(window)))
    {
        for (const auto &callback : self->_scrollCallbacks)
            callback(xOffset, yOffset);
    }
}

void WindowContext::WindowRefreshTrampoline(GLFWwindow *window)
{
    if (auto *self = static_cast<WindowContext *>(glfwGetWindowUserPointer(window)))
    {
        for (const auto &callback : self->_windowRefreshCallbacks)
            callback();
    }
}

void WindowContext::OnFramebufferSize(std::function<void(int, int)> callback)
{
    _framebufferSizeCallbacks.push_back(std::move(callback));
}

void WindowContext::OnScroll(std::function<void(double, double)> callback)
{
    _scrollCallbacks.push_back(std::move(callback));
}

void WindowContext::OnWindowRefresh(std::function<void()> callback)
{
    _windowRefreshCallbacks.push_back(std::move(callback));
}

WindowContext::~WindowContext()
{
    if (_nativeWindowHandle != nullptr)
    {
        glfwDestroyWindow(_nativeWindowHandle);
    }
}

WindowContext::WindowContext(WindowContext &&other) noexcept
    : _glfwLibrary(std::move(other._glfwLibrary)), _nativeWindowHandle(other._nativeWindowHandle),
    _isValid(other._isValid),
    _framebufferSizeCallbacks(std::move(other._framebufferSizeCallbacks)),
    _scrollCallbacks(std::move(other._scrollCallbacks)),
    _windowRefreshCallbacks(std::move(other._windowRefreshCallbacks))
{
    other._nativeWindowHandle = nullptr;
    other._isValid = false;

    // The GLFW user pointer still points at 'other'; re-seat it on this object
    // so the callback trampolines dispatch to the moved-to subscriber lists.
    if (_nativeWindowHandle != nullptr)
    {
        glfwSetWindowUserPointer(_nativeWindowHandle, this);
    }
}

WindowContext &WindowContext::operator=(WindowContext &&other) noexcept
{
    if (this != &other)
    {
        if (_nativeWindowHandle != nullptr)
        {
            glfwDestroyWindow(_nativeWindowHandle);
        }

        _glfwLibrary = std::move(other._glfwLibrary);
        _nativeWindowHandle = other._nativeWindowHandle;
        _isValid = other._isValid;
        _framebufferSizeCallbacks = std::move(other._framebufferSizeCallbacks);
        _scrollCallbacks = std::move(other._scrollCallbacks);
        _windowRefreshCallbacks = std::move(other._windowRefreshCallbacks);

        other._nativeWindowHandle = nullptr;
        other._isValid = false;

        if (_nativeWindowHandle != nullptr)
        {
            glfwSetWindowUserPointer(_nativeWindowHandle, this);
        }
    }

    return *this;
}

bool WindowContext::IsValid() const
{
    return _isValid;
}

NativeWindowHandle *WindowContext::NativeHandle() const
{
    return _nativeWindowHandle;
}

void WindowContext::PollEvents()
{
    glfwPollEvents();
}

// The GLFW calls below dereference the window handle unchecked, so each entry
// point guards against a failed-construction or moved-from WindowContext
// (null handle) rather than trusting every caller to check IsValid() first.

bool WindowContext::ShouldClose() const
{
    // A window that doesn't exist reads as "close": callers loop on
    // !ShouldClose(), and spinning forever on a dead window is the worse bug.
    return _nativeWindowHandle == nullptr || glfwWindowShouldClose(_nativeWindowHandle) != 0;
}

bool WindowContext::IsFocused() const
{
    return _nativeWindowHandle != nullptr && glfwGetWindowAttrib(_nativeWindowHandle, GLFW_FOCUSED) != 0;
}

void WindowContext::RequestClose() const
{
    if (_nativeWindowHandle == nullptr)
    {
        return;
    }
    glfwSetWindowShouldClose(_nativeWindowHandle, GLFW_TRUE);
}

void WindowContext::SetTitle(const std::string &title) const
{
    if (_nativeWindowHandle == nullptr)
    {
        return;
    }
    glfwSetWindowTitle(_nativeWindowHandle, title.c_str());
}

WindowSize WindowContext::GetWindowSize() const
{
    int windowWidth = 0;
    int windowHeight = 0;

    if (_nativeWindowHandle != nullptr)
    {
        glfwGetWindowSize(_nativeWindowHandle, &windowWidth, &windowHeight);
    }

    WindowSize result;
    result.Width = windowWidth;
    result.Height = windowHeight;
    return result;
}

WindowSize WindowContext::GetFramebufferSize() const
{
    int framebufferWidth = 0;
    int framebufferHeight = 0;

    if (_nativeWindowHandle != nullptr)
    {
        glfwGetFramebufferSize(_nativeWindowHandle, &framebufferWidth, &framebufferHeight);
    }

    WindowSize result;
    result.Width = framebufferWidth;
    result.Height = framebufferHeight;
    return result;
}
} /* namespace Assisi::Window */
