#pragma once

/// @file WindowContext.hpp
/// @brief Owns a single GLFW window and its associated OpenGL context.
///
/// `WindowContext` wraps window creation, context management, swap-chain
/// control, and event polling into one RAII object.  It holds a shared
/// reference to `GlfwLibrary` so GLFW cannot be terminated while any
/// window is still alive.

#include <memory>
#include <string>

#include <Assisi/Window/GlfwLibrary.hpp>

struct GLFWwindow;
using GLFWframebuffersizefun = void (*)(GLFWwindow *, int, int);

namespace Assisi::Window
{
using NativeWindowHandle = GLFWwindow;

/// @brief Width and height of a window or framebuffer, in pixels.
struct WindowSize
{
    int Width = 0;  ///< Pixel width.
    int Height = 0; ///< Pixel height.
};

/// @brief Parameters passed to the WindowContext constructor.
struct WindowConfiguration
{
    int Width = 1280;             ///< Initial window width in pixels.
    int Height = 720;             ///< Initial window height in pixels.
    const char *Title = "Assisi"; ///< Window title bar text.
    bool EnableVSync = true;      ///< Whether to enable vertical synchronisation.

    /// @brief When false, GLFW skips client API context creation (e.g. for Vulkan).
    bool CreateClientApiContext = true;
};

/// @brief RAII owner of a GLFW window and its rendering context.
///
/// Only one context should be current on a thread at a time.  Move semantics
/// transfer window ownership; copying is disabled.
class WindowContext
{
  public:
    /// @brief Creates the GLFW window and makes its context current.
    ///
    /// On failure (GLFW not initialised, or window creation error) the object
    /// is left in an invalid state — check IsValid() before use.
    ///
    /// @param configuration    Window dimensions, title, and feature flags.
    /// @param framebufferSizeCallback  Optional callback invoked when the
    ///        framebuffer is resized.  Pass nullptr to skip registration.
    WindowContext(const WindowConfiguration &configuration, GLFWframebuffersizefun framebufferSizeCallback);

    /// @brief Destroys the underlying GLFW window.
    ~WindowContext();

    WindowContext(const WindowContext &) = delete;
    WindowContext &operator=(const WindowContext &) = delete;

    /// @brief Transfers window ownership; the moved-from object becomes invalid.
    WindowContext(WindowContext &&other) noexcept;

    /// @brief Transfers window ownership, destroying the current window first.
    WindowContext &operator=(WindowContext &&other) noexcept;

    /// @brief Returns true if the window and context were created successfully.
    [[nodiscard]] bool IsValid() const;

    /// @brief Returns the underlying GLFWwindow pointer.
    [[nodiscard]] NativeWindowHandle *NativeHandle() const;

    /// @brief Processes pending OS events.
    static void PollEvents();

    /// @brief Returns true if the user or OS has requested the window to close.
    [[nodiscard]] bool ShouldClose() const;

    /// @brief Flags the window for closure; ShouldClose() will return true afterward.
    void RequestClose() const;

    /// @brief Presents the back buffer (swap buffers).
    void SwapBuffers() const;

    /// @brief Updates the window title bar text.
    void SetTitle(const std::string &title) const;

    /// @brief Returns true if vertical synchronisation is currently enabled.
    [[nodiscard]] bool IsVSyncEnabled() const;

    /// @brief Enables or disables vertical synchronisation.
    ///
    /// Makes the window context current before changing the swap interval so
    /// the setting is applied to the correct context in multi-window setups.
    ///
    /// @param enabled  True to enable VSync (swap interval 1), false to disable (0).
    void SetVSyncEnabled(bool enabled);

    /// @brief Returns the current window size in screen coordinates.
    ///
    /// On high-DPI displays this may differ from GetFramebufferSize().
    [[nodiscard]] WindowSize GetWindowSize() const;

    /// @brief Returns the framebuffer size in pixels.
    ///
    /// Use this for viewport and projection calculations rather than
    /// GetWindowSize(), as they differ on HiDPI / Retina screens.
    [[nodiscard]] WindowSize GetFramebufferSize() const;

  private:
    /// @brief Keeps GLFW alive for at least as long as this window.
    std::shared_ptr<GlfwLibrary> _glfwLibrary;

    /// @brief The underlying GLFW window handle.  Null when invalid or moved-from.
    NativeWindowHandle *_nativeWindowHandle = nullptr;

    /// @brief True after successful window creation.
    bool _isValid = false;

    /// @brief Mirrors the current swap-interval setting for IsVSyncEnabled().
    bool _isVSyncEnabled = false;
};
} /* namespace Assisi::Window */
