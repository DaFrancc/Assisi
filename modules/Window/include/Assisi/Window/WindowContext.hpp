#pragma once

#ifndef WINDOWCONTEXT_HPP
#define WINDOWCONTEXT_HPP

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <iostream>
#include <memory>
#include <string>

#include <Assisi/Window/GlfwLibrary.hpp>

namespace Assisi::Window
{
using NativeWindowHandle = GLFWwindow;

struct WindowSize
{
    int Width = 0;
    int Height = 0;
};

struct WindowConfiguration
{
    int Width = 1280;
    int Height = 720;
    const char *Title = "Assisi";
    bool EnableVSync = true;

    /* If false, GLFW will not create any client API context (e.g. Vulkan). */
    bool CreateClientApiContext = true;
};

class WindowContext
{
  public:
    WindowContext(const WindowConfiguration &configuration, GLFWframebuffersizefun framebufferSizeCallback)
        : _glfwLibrary(GlfwLibrary::Acquire()), _isVSyncEnabled(configuration.EnableVSync)
    {
        if (!_glfwLibrary || !_glfwLibrary->IsValid())
        {
            return;
        }

        /* Optionally disable client API context creation. */
        if (!configuration.CreateClientApiContext)
        {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        }

        /* Create the GLFW window. */
        _nativeWindowHandle =
            glfwCreateWindow(configuration.Width, configuration.Height, configuration.Title, nullptr, nullptr);

        if (_nativeWindowHandle == nullptr)
        {
            std::cout << "Failed to create GLFW window." << std::endl;
            return;
        }

        /* Make context current (only meaningful for some backends). */
        glfwMakeContextCurrent(_nativeWindowHandle);

        /* Apply vertical sync preference. */
        glfwSwapInterval(_isVSyncEnabled ? 1 : 0);

        /* Install framebuffer resize callback. */
        if (framebufferSizeCallback != nullptr)
        {
            glfwSetFramebufferSizeCallback(_nativeWindowHandle, framebufferSizeCallback);
        }

        _isValid = true;
    }

    ~WindowContext()
    {
        if (_nativeWindowHandle != nullptr)
        {
            glfwDestroyWindow(_nativeWindowHandle);
        }
    }

    WindowContext(const WindowContext &) = delete;
    WindowContext &operator=(const WindowContext &) = delete;

    WindowContext(WindowContext &&other) noexcept
        : _glfwLibrary(std::move(other._glfwLibrary)), _nativeWindowHandle(other._nativeWindowHandle),
          _isValid(other._isValid), _isVSyncEnabled(other._isVSyncEnabled)
    {
        other._nativeWindowHandle = nullptr;
        other._isValid = false;
        other._isVSyncEnabled = false;
    }

    WindowContext &operator=(WindowContext &&other) noexcept
    {
        if (this != &other)
        {
            /* Destroy any currently owned window. */
            if (_nativeWindowHandle != nullptr)
            {
                glfwDestroyWindow(_nativeWindowHandle);
            }

            _glfwLibrary = std::move(other._glfwLibrary);
            _nativeWindowHandle = other._nativeWindowHandle;
            _isValid = other._isValid;
            _isVSyncEnabled = other._isVSyncEnabled;

            other._nativeWindowHandle = nullptr;
            other._isValid = false;
            other._isVSyncEnabled = false;
        }

        return *this;
    }

    bool IsValid() const { return _isValid; }

    NativeWindowHandle *NativeHandle() const { return _nativeWindowHandle; }

    void PollEvents() const { glfwPollEvents(); }

    bool ShouldClose() const { return glfwWindowShouldClose(_nativeWindowHandle); }

    void RequestClose() const { glfwSetWindowShouldClose(_nativeWindowHandle, GLFW_TRUE); }

    void SwapBuffers() const { glfwSwapBuffers(_nativeWindowHandle); }

    void SetTitle(const std::string &title) const { glfwSetWindowTitle(_nativeWindowHandle, title.c_str()); }

    bool IsVSyncEnabled() const { return _isVSyncEnabled; }

    void SetVSyncEnabled(bool enabled)
    {
        _isVSyncEnabled = enabled;

        /* Swap interval is per-context; ensure the correct window is current. */
        glfwMakeContextCurrent(_nativeWindowHandle);
        glfwSwapInterval(_isVSyncEnabled ? 1 : 0);
    }

    WindowSize GetWindowSize() const
    {
        int windowWidth = 0;
        int windowHeight = 0;

        glfwGetWindowSize(_nativeWindowHandle, &windowWidth, &windowHeight);

        WindowSize result;
        result.Width = windowWidth;
        result.Height = windowHeight;
        return result;
    }

    WindowSize GetFramebufferSize() const
    {
        int framebufferWidth = 0;
        int framebufferHeight = 0;

        glfwGetFramebufferSize(_nativeWindowHandle, &framebufferWidth, &framebufferHeight);

        WindowSize result;
        result.Width = framebufferWidth;
        result.Height = framebufferHeight;
        return result;
    }

  private:
    std::shared_ptr<GlfwLibrary> _glfwLibrary;
    NativeWindowHandle *_nativeWindowHandle = nullptr;

    bool _isValid = false;
    bool _isVSyncEnabled = false;
};
} /* namespace Assisi::Window */

#endif /* WINDOWCONTEXT_HPP */