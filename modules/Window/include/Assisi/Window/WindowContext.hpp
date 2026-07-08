/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WindowContext.hpp
/// @brief Owns a single GLFW window, created with no client API (GLFW_NO_API) —
/// Vulkan owns the swapchain/presentation via VulkanContext, not GLFW.
///
/// `WindowContext` wraps window creation, event polling, and swap-interval
/// preference into one RAII object.  It holds a shared reference to
/// `GlfwLibrary` so GLFW cannot be terminated while any window is still alive.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <Assisi/Window/GlfwLibrary.hpp>

struct GLFWwindow;

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
};

/// @brief RAII owner of a GLFW window and the single source of truth for its
/// GLFW callbacks.
///
/// WindowContext owns the window's GLFW user pointer and installs the process's
/// GLFW callbacks itself, then fans each event out to any number of subscribers
/// registered via On*(). This avoids the classic footgun of multiple systems
/// (input, ImGui, the app) each calling glfwSet*Callback and clobbering one
/// another. Dear ImGui's backend is initialised *after* this window with
/// install_callbacks=true, so it saves these callbacks and chains to them —
/// meaning ImGui and the engine's subscribers both receive input.
///
/// @warning Subscribers must outlive the WindowContext (or at least outlive any
/// event pumping): callbacks fire only during PollEvents(), so it is safe for a
/// subscriber to be destroyed after the run loop stops, but not to be polled
/// after it dies. There is no unsubscribe — the expected lifetimes are strictly
/// nested (Application owns the window and everything that subscribes to it).
///
/// Only one context should be current on a thread at a time.  Move semantics
/// transfer window ownership; copying is disabled.
class WindowContext
{
  public:
    /// @brief Creates the GLFW window and installs its GLFW callbacks.
    ///
    /// On failure (GLFW not initialised, or window creation error) the object
    /// is left in an invalid state — check IsValid() before use.
    ///
    /// @param configuration    Window dimensions, title, and feature flags.
    explicit WindowContext(const WindowConfiguration &configuration);

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

    // -------------------------------------------------------------------------
    // Event subscription — see the class @warning about subscriber lifetimes.
    // Subscribers are invoked in registration order.
    // -------------------------------------------------------------------------

    /// @brief Subscribe to framebuffer-resize events (new size in pixels).
    void OnFramebufferSize(std::function<void(int width, int height)> callback);

    /// @brief Subscribe to scroll-wheel events (x/y offsets, y is the vertical wheel).
    void OnScroll(std::function<void(double xOffset, double yOffset)> callback);

    /// @brief Subscribe to window-refresh events (the OS requests a redraw, e.g.
    /// during a live resize/move where the main loop is otherwise blocked).
    void OnWindowRefresh(std::function<void()> callback);

  private:
    /// @brief Points the window's GLFW user pointer at this object and installs
    /// the GLFW callbacks that fan out to subscribers. Called on construction
    /// and after a move (which re-seats the user pointer on the new owner).
    void InstallCallbacks();

    // GLFW C-callback trampolines: recover the WindowContext from the user
    // pointer and dispatch to the subscriber lists.
    static void FramebufferSizeTrampoline(GLFWwindow *window, int width, int height);
    static void ScrollTrampoline(GLFWwindow *window, double xOffset, double yOffset);
    static void WindowRefreshTrampoline(GLFWwindow *window);

    /// @brief Keeps GLFW alive for at least as long as this window.
    std::shared_ptr<GlfwLibrary> _glfwLibrary;

    /// @brief The underlying GLFW window handle.  Null when invalid or moved-from.
    NativeWindowHandle *_nativeWindowHandle = nullptr;

    /// @brief True after successful window creation.
    bool _isValid = false;

    /// @brief Mirrors the current vsync preference for IsVSyncEnabled(). Not yet
    /// wired to the Vulkan swapchain's present mode — see
    /// docs/nvrhi-migration-todo.md.
    bool _isVSyncEnabled = false;

    std::vector<std::function<void(int, int)>>     _framebufferSizeCallbacks;
    std::vector<std::function<void(double, double)>> _scrollCallbacks;
    std::vector<std::function<void()>>             _windowRefreshCallbacks;
};
} /* namespace Assisi::Window */
