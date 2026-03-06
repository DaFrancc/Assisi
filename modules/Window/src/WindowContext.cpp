#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Window/WindowContext.hpp>

namespace Assisi::Window
{
WindowContext::WindowContext(const WindowConfiguration &configuration, GLFWframebuffersizefun framebufferSizeCallback)
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
    else
    {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    }

    /* Create the GLFW window. */
    _nativeWindowHandle =
        glfwCreateWindow(configuration.Width, configuration.Height, configuration.Title, nullptr, nullptr);

    if (_nativeWindowHandle == nullptr)
    {
        Assisi::Core::Log::Error("Failed to create GLFW window.");
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

WindowContext::~WindowContext()
{
    if (_nativeWindowHandle != nullptr)
    {
        glfwDestroyWindow(_nativeWindowHandle);
    }
}

WindowContext::WindowContext(WindowContext &&other) noexcept
    : _glfwLibrary(std::move(other._glfwLibrary)), _nativeWindowHandle(other._nativeWindowHandle),
      _isValid(other._isValid), _isVSyncEnabled(other._isVSyncEnabled)
{
    other._nativeWindowHandle = nullptr;
    other._isValid = false;
    other._isVSyncEnabled = false;
}

WindowContext &WindowContext::operator=(WindowContext &&other) noexcept
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

bool WindowContext::ShouldClose() const
{
    return glfwWindowShouldClose(_nativeWindowHandle) != 0;
}

void WindowContext::RequestClose() const
{
    glfwSetWindowShouldClose(_nativeWindowHandle, GLFW_TRUE);
}

void WindowContext::SwapBuffers() const
{
    glfwSwapBuffers(_nativeWindowHandle);
}

void WindowContext::SetTitle(const std::string &title) const
{
    glfwSetWindowTitle(_nativeWindowHandle, title.c_str());
}

bool WindowContext::IsVSyncEnabled() const
{
    return _isVSyncEnabled;
}

void WindowContext::SetVSyncEnabled(bool enabled)
{
    _isVSyncEnabled = enabled;

    /* Swap interval is per-context; ensure the correct window is current. */
    glfwMakeContextCurrent(_nativeWindowHandle);
    glfwSwapInterval(_isVSyncEnabled ? 1 : 0);
}

WindowSize WindowContext::GetWindowSize() const
{
    int windowWidth = 0;
    int windowHeight = 0;

    glfwGetWindowSize(_nativeWindowHandle, &windowWidth, &windowHeight);

    WindowSize result;
    result.Width = windowWidth;
    result.Height = windowHeight;
    return result;
}

WindowSize WindowContext::GetFramebufferSize() const
{
    int framebufferWidth = 0;
    int framebufferHeight = 0;

    glfwGetFramebufferSize(_nativeWindowHandle, &framebufferWidth, &framebufferHeight);

    WindowSize result;
    result.Width = framebufferWidth;
    result.Height = framebufferHeight;
    return result;
}
} /* namespace Assisi::Window */
