#pragma once

/// @file Application.hpp
/// @brief Base class for all Assisi applications. Derive from Application,
///        override the hooks, and call Run() from main().

#include <Assisi/App/AppConfig.hpp>
#include <Assisi/App/OptionsConfig.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/PostProcess.hpp>
#include <Assisi/Render/Vulkan/VulkanContext.hpp>
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
///   - OnRender(Render::Vulkan::VulkanFrame&) — color/depth targets are already
///     cleared, called between BeginFrame() and EndFrame(). When an
///     anti-aliasing mode is active (see F12 options), `frame` points at an
///     offscreen target instead of the swapchain — PostProcess resolves it
///     into the swapchain afterwards, transparently to this override.
///
/// Optional overrides (no-ops by default):
///   - OnImGui()                 — called after OnRender(), inside the same
///     ImGui frame DebugUI opens; build ImGui:: windows here
///   - OnResize(int, int)        — called when the framebuffer is resized
///   - OnRenderTargetsChanged(const nvrhi::FramebufferInfo&) — called whenever
///     the FramebufferInfo OnRender()'s `frame` will be compatible with next
///     changes (i.e. its sample count) — rebuild any graphics pipelines built
///     against the old one (see Render::MeshPass::RebuildPipeline)
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
    virtual void OnRender(Render::Vulkan::VulkanFrame &frame) = 0;
    virtual void OnImGui()                  {}
    virtual void OnShutdown()               {}
    /// @brief Called when the framebuffer is resized. Override to react to resolution changes.
    virtual void OnResize(int /*width*/, int /*height*/) {}
    /// @brief Called after the anti-aliasing mode/MSAA sample count changes
    /// (F12 options window), before the next OnRender(). Only fires when the
    /// new FramebufferInfo actually differs from the previous one — resizing
    /// the window alone never triggers this.
    virtual void OnRenderTargetsChanged(const nvrhi::FramebufferInfo & /*framebufferInfo*/) {}

    Window::WindowContext &GetWindow() const { return *_window; }
    Window::InputContext  &GetInput()  const { return *_input; }

    void      RequestClose();
    glm::mat4 MakeProjection(float fovDegrees = 60.f, float zNear = 0.1f, float zFar = 200.f) const;
    int       GetFps()             const { return _fps; }

    /// @brief The FramebufferInfo OnRender()'s `frame` is (or will be, at the
    /// next OnRender()) compatible with. Build scene pipelines against this,
    /// not the swapchain's own FramebufferInfo directly, so they're already
    /// correct if an anti-aliasing mode is active from a saved options.json.
    nvrhi::FramebufferInfo GetSceneFramebufferInfo() const { return _postProcess.SceneFramebufferInfo(); }

  private:
    static void FramebufferSizeCallback(Window::NativeWindowHandle *window, int width, int height);
    static void WindowRefreshCallback(Window::NativeWindowHandle *window);
    void        RenderFrame();
    void        ConfigurePostProcess();
    void        DrawOptionsWindow();

    AppConfig     _config;
    OptionsConfig _options;

    std::unique_ptr<Window::WindowContext> _window;
    std::unique_ptr<Window::InputContext>  _input;

    Render::PostProcess _postProcess;
    bool                _showOptionsWindow = false;

    int _fps = 0;
};

} // namespace Assisi::App
