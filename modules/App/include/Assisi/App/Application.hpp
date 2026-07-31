/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Application.hpp
/// @brief Base class for all Assisi applications. Derive from Application,
///        override the hooks, and call Run() from main().

#include <Assisi/App/AppConfig.hpp>
#include <Assisi/App/OptionsConfig.hpp>
#include <Assisi/Chiara/Chiara.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/JobSystem.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/PostProcess.hpp>
#include <Assisi/Render/Vulkan/VulkanContext.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/WindowContext.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace Assisi::App
{

/// @brief Base class for all Assisi applications.
///
/// Required overrides:
///   - OnStart()
///   - OnFixedUpdate(float dt)   — called at physicsHz
///   - OnUpdate(float dt)        — called every render frame
///   - OnRender(Render::RenderFrame&) — color/depth targets are already
///     cleared, called between BeginFrame() and EndFrame(). When an
///     anti-aliasing mode is active (see F11 options), `frame` points at an
///     offscreen target instead of the swapchain — PostProcess resolves it
///     into the swapchain afterwards, transparently to this override.
///
/// Optional overrides (no-ops by default):
///   - OnImGui()                 — called after OnRender(), inside the same
///     ImGui frame DebugUI opens; build ImGui:: windows here
///   - OnResize(int32_t, int32_t) — called when the framebuffer is resized
///   - OnRenderTargetsChanged(const nvrhi::FramebufferInfo&) — called whenever
///     the FramebufferInfo that OnRender()'s `frame` is compatible with changes
///     (e.g. its sample count) — rebuild any graphics pipelines built against
///     the old one (see Render::MeshPass::RebuildPipeline)
///   - OnShutdown()              — called after the loop exits
class Application
{
  public:
    Application();
    virtual ~Application();

    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    /// @brief Brings up the engine (asset system, window, renderer, ImGui,
    /// input, post-process). Must be called once, after construction and before
    /// Run().
    ///
    /// @return true on success; false if any bring-up step failed (the failing
    /// step logs the reason). On failure the object is safe to destroy — no
    /// std::exit — so callers should simply return from main().
    ///
    /// @note Error convention: the App/Render/Window layers report fallible
    /// operations as `bool` + a log at the failure site; Core uses
    /// std::expected. Bring-up failures are already logged in detail here, so a
    /// bare bool is enough for main() to decide to bail.
    [[nodiscard]] bool Initialize();

    void Run();

  protected:
    virtual void OnStart()                  = 0;
    virtual void OnFixedUpdate(float dt)    = 0;
    virtual void OnUpdate(float dt)         = 0;
    virtual void OnRender(Render::RenderFrame &frame) = 0;
    virtual void OnImGui()                  {}
    virtual void OnShutdown()               {}
    /// @brief Called when the framebuffer is resized. Override to react to resolution changes.
    virtual void OnResize(int32_t /*width*/, int32_t /*height*/) {}
    /// @brief Called after the anti-aliasing mode/MSAA sample count changes
    /// (F11 options window), before the next OnRender(). Only fires when the
    /// new FramebufferInfo actually differs from the previous one — resizing
    /// the window alone never triggers this.
    virtual void OnRenderTargetsChanged(const nvrhi::FramebufferInfo & /*framebufferInfo*/) {}

    /// @brief Called once per frame at end of frame (after RenderFrame and the
    /// event-queue flush) to apply deferred per-frame work. Override to drain the
    /// app's active scene(s) — notably ECS::Scene::FlushDestroyed(), where queued
    /// entity destruction is applied so it never mutates pools mid-Query. Default
    /// is a no-op: Application owns no scene, so the app must opt in.
    virtual void FlushDeferred() {}

    Window::WindowContext &GetWindow() const { return *_window; }
    Window::InputContext  &GetInput()  const { return *_input; }

    /// @brief Fraction of a fixed physics step left unconsumed by the current
    /// frame — in [0, 1). Use it in OnRender() to blend physics-driven state
    /// between its previous and current fixed-step poses (see
    /// Physics::PhysicsWorld::InterpolateTransforms), so motion stays smooth
    /// when the display refreshes faster than physics steps. Recomputed every
    /// frame after the fixed-update loop and stable through OnUpdate/OnRender.
    float GetInterpolationAlpha() const { return _interpolationAlpha; }

    /// @brief The per-frame event queue, owned by Application and flushed once
    /// per frame in Run(). Systems normally reach it through SystemContext
    /// (ctx.events); this accessor is for app code outside a system.
    Core::EventQueue &GetEvents() { return _events; }

    /// @brief The engine's shared task scheduler (design:
    /// docs/job-system-design-notes.md). Owned by Application; its main-thread
    /// queue is drained once per frame in Run() just before OnUpdate. Use
    /// Jobs().RunOnMain(...) to marshal background results back to a safe point,
    /// ParallelFor(...) to fan work out, or Run(pool, fn) for async chains.
    Core::JobSystem &Jobs() { return _jobs; }

    /// @brief Cap on main-thread tasks drained per frame (0 = unbounded, the
    /// default). Async results publish on the main queue, and a streaming asset
    /// publish does real GPU work — createTexture + writeTexture + a blocking
    /// executeCommandList per texture/mesh. Draining a whole burst in one frame
    /// spikes it; lowering this budget spreads those publishes across frames so a
    /// background load doesn't hitch the running frame, at the cost of the results
    /// landing a few frames later. Clear it (0) when the burst is over.
    void SetMainThreadTaskBudget(uint32_t perFrame) { _mainThreadTaskBudget = perFrame; }
    [[nodiscard]] uint32_t GetMainThreadTaskBudget() const { return _mainThreadTaskBudget; }

    void      RequestClose();
    int32_t   GetFps()             const { return _fps; }

    /// @brief Averaged CPU main-thread work per frame, in milliseconds —
    /// excluding the FPS-limit pacing sleep and time spent blocked on the GPU.
    /// Compare against GetGpuFrameMs() to see which side is the bottleneck.
    double    GetCpuFrameMs()      const { return _cpuFrameMs; }

    /// @brief Averaged GPU execution time per frame, in milliseconds, from a
    /// timer query spanning the whole command list.
    double    GetGpuFrameMs()      const { return _gpuFrameMs; }

    /// @brief The FramebufferInfo OnRender()'s `frame` is (or will be, at the
    /// next OnRender()) compatible with. Build scene pipelines against this,
    /// not the swapchain's own FramebufferInfo directly, so they're already
    /// correct if an anti-aliasing mode is active from a saved options.json.
    nvrhi::FramebufferInfo GetSceneFramebufferInfo() const { return _postProcess.SceneFramebufferInfo(); }

    /// @brief The persisted user options (AA mode, MSAA samples, frame-sync
    /// mode, FPS cap) the engine consumes each frame in Run(). Exposed mutable
    /// so an app-side options overlay can edit them; after changing aaMode or
    /// msaaSamples, call ApplyDisplayOptions() to rebuild the render targets,
    /// and OptionsConfig::SaveToJson() to persist. Frame-sync/FPS changes are
    /// picked up by Run() on the next iteration with no extra call.
    OptionsConfig &GetOptions() { return _options; }

    /// @brief The engine config loaded at Initialize() (game.json): window
    /// title/size, physics rate. Read-only — it reflects bring-up, and editing
    /// it after the fact would change nothing.
    const AppConfig &GetConfig() const { return _config; }

    /// @brief Rebuilds the post-process render targets from the current
    /// _options.aaMode / msaaSamples (and fires OnRenderTargetsChanged if the
    /// FramebufferInfo actually changed). Call after editing those options.
    void ApplyDisplayOptions() { ConfigurePostProcess(); }

    /// @brief A read-only view of the rolling per-frame timing history, for an
    /// app-side debug overlay. Each array is a ring buffer of FrameHistory()
    /// samples; `offset` is the oldest sample / next slot to overwrite, and
    /// `sampleCount` saturates at the capacity once the buffer has filled.
    struct FrameStatsView
    {
        std::span<const float> cpuMs;
        std::span<const float> gpuMs;
        std::span<const float> frameDeltaMs;
        int32_t                offset;
        int32_t                sampleCount;
    };
    FrameStatsView GetFrameStats() const
    {
        return {_cpuHistory, _gpuHistory, _frameTimeHistory, _frameHistoryOffset, _frameSampleCount};
    }
    static constexpr int32_t FrameHistory() { return kFrameHistory; }

  private:
    void HandleFramebufferResize(int32_t width, int32_t height);
    void RenderFrame();
    void ConfigurePostProcess();

    /// Declared first so the capture runtime is up before anything else exists —
    /// in particular before _jobs spawns its workers, which register themselves
    /// with it — and so it is torn down last. Inert and empty unless built with
    /// -c (see docs/chiara-design-notes.md).
    Chiara::InitGuard _chiara;

    AppConfig     _config;
    OptionsConfig _options;

    std::unique_ptr<Window::WindowContext> _window;
    std::unique_ptr<Window::InputContext>  _input;

    // Declared before the subsystems (post-process, and the derived app's caches)
    // so it is destroyed last: workers join only after everything that might have
    // scheduled work is already gone. Derived-class members are destroyed before
    // this base's, so the app's AssetCache etc. can safely use it up to teardown.
    Core::JobSystem _jobs;

    Render::PostProcess _postProcess;
    Core::EventQueue    _events;
    bool                _initialized = false;

    int32_t _fps = 0;
    double  _cpuFrameMs = 0.0;
    double  _gpuFrameMs = 0.0;

    // Leftover accumulator as a fraction of a physics step, in [0, 1). Set once
    // per frame after the fixed-update loop; read by OnRender via
    // GetInterpolationAlpha() to blend physics state between fixed steps.
    float _interpolationAlpha = 0.0f;

    // Per-frame cap passed to JobSystem::DrainMain (0 = unbounded). See
    // SetMainThreadTaskBudget: throttles streaming asset publishes so a burst of
    // GPU uploads spreads across frames instead of hitching one.
    uint32_t _mainThreadTaskBudget = 0;

    // Rolling per-frame samples (milliseconds) for the ImGui plots and the
    // percentile stats, kept as ring buffers: _frameHistoryOffset is the next
    // slot to overwrite, which is also the oldest sample — the values_offset
    // ImGui::PlotLines() wants. _frameSampleCount saturates at kFrameHistory so
    // the stats ignore the zero-filled slots before the buffer first fills.
    static constexpr int32_t          kFrameHistory = 360;
    std::array<float, kFrameHistory>  _cpuHistory{};
    std::array<float, kFrameHistory>  _gpuHistory{};
    std::array<float, kFrameHistory>  _frameTimeHistory{}; // full frame delta, for 1%-low etc.
    int32_t                           _frameHistoryOffset = 0;
    int32_t                           _frameSampleCount = 0;

    // RenderFrame's sub-phase breakdown used to live here as a struct of doubles
    // scraped into the slow-frame log line. It is now profile scopes inside
    // RenderFrame — same numbers, but scrubbable, nested under the frame, and
    // costing nothing in a build without capture.
};

} // namespace Assisi::App
