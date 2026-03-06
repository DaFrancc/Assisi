#pragma once

/// @file Application.hpp
/// @brief Base class for all Assisi applications. Derive from Application,
///        override the hooks, and call Run() from main().

#include <Assisi/App/AppConfig.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/WindowContext.hpp>

#include <memory>

namespace Assisi::App
{

/// @brief Base class for all Assisi applications.
///
/// Required overrides:
///   - OnStart()
///   - OnFixedUpdate(float dt)   — called at physicsHz
///   - OnUpdate(float dt)        — called every render frame
///   - OnRender()                — framebuffer is already cleared
///
/// Optional overrides (no-ops by default):
///   - OnImGui()                 — called inside an ImGui frame
///   - OnShutdown()              — called after the loop exits
class Application
{
  public:
    Application();
    virtual ~Application();

    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    void Run();

  protected:
    virtual void OnStart()                  = 0;
    virtual void OnFixedUpdate(float dt)    = 0;
    virtual void OnUpdate(float dt)         = 0;
    virtual void OnRender()                 = 0;
    virtual void OnImGui()                  {}
    virtual void OnShutdown()               {}
    /// @brief Called when the framebuffer is resized. Override to react to resolution changes.
    virtual void OnResize(int /*width*/, int /*height*/) {}

    Window::WindowContext &GetWindow() const { return *_window; }
    Window::InputContext  &GetInput()  const { return *_input; }

    void      RequestClose();
    glm::mat4 MakeProjection(float fovDegrees = 60.f, float zNear = 0.1f, float zFar = 200.f) const;
    int       GetFps()             const { return _fps; }
    double    GetSleepResolutionMs() const { return _sleepResolutionMs; }

  private:
    static void FramebufferSizeCallback(Window::NativeWindowHandle *window, int width, int height);
    static void WindowRefreshCallback(Window::NativeWindowHandle *window);
    void        RenderFrame();

    AppConfig _config;

    std::unique_ptr<Window::WindowContext> _window;
    std::unique_ptr<Window::InputContext>  _input;

    int    _fps               = 0;
    double _sleepResolutionMs = 0.0;
};

} // namespace Assisi::App