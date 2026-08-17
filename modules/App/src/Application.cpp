/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file Application.cpp

// --- Platform timer (must come before other Windows headers) ----------------
#ifdef _WIN32
#    include <windows.h>
#    include <timeapi.h>
#    pragma comment(lib, "winmm.lib")
#endif

// --- Engine headers ---------------------------------------------------------
#include <Assisi/App/Application.hpp>
#include <Assisi/App/CrashReport.hpp>
#include <Assisi/Core/Diagnostics.hpp>

#include <Assisi/Chiara/Profile.hpp>
#include <Assisi/Chiara/Serializer.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Sinks.hpp>
#include <Assisi/Core/Platform.hpp>
#include <Assisi/Debug/DebugUI.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>
#include <Assisi/Render/GpuMarker.hpp>
#include <Assisi/Render/RenderSystem.hpp>

// --- Standard ---------------------------------------------------------------
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <thread>

namespace Assisi::App
{

void Application::HandleFramebufferResize(int32_t width, int32_t height)
{
    // GLFW can fire this during window creation/show (e.g. a DPI-driven WM_SIZE
    // on Windows). Bail on a zero-size (minimized) framebuffer, and no-op safely
    // if the render backend isn't up yet (GetVulkanContext() returns null).
    if (width <= 0 || height <= 0)
    {
        return;
    }

    if (auto *vulkanContext = Render::RenderSystem::GetVulkanContext())
    {
        vulkanContext->Resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }

    OnResize(width, height);
    ConfigurePostProcess();
}


#ifdef _WIN32
struct TimerResolutionScope
{
    TimerResolutionScope()  { timeBeginPeriod(1); }
    ~TimerResolutionScope() { timeEndPeriod(1); }
    TimerResolutionScope(const TimerResolutionScope &) = delete;
    TimerResolutionScope &operator=(const TimerResolutionScope &) = delete;
};
#endif

// ---------------------------------------------------------------------------

// Ceiling applied before game.json is available. High enough that it can never
// trim below a configured keepLogs/keepDumps, low enough to bound a directory
// on a build that never reaches InitializeCore.
constexpr uint32_t kRetentionBackstop = 50;

Application::Application()
{
    // Only infallible setup belongs in the constructor. Logging is wired up first
    // so log lines from derived-class member constructors (which run after this
    // base ctor) are captured; all fallible bring-up lives in Initialize(), so a
    // failure unwinds normally instead of std::exit()-ing past every destructor.
    //
    // Log and crash report are writable runtime outputs, resolved under the user
    // root (defaults to the exe dir) so they don't depend on the CWD the process
    // was launched from. The user root initializes lazily here, before
    // AssetSystem::Initialize() discovers the read-only asset root.
    //
    // A console sink only when there is somewhere for it to go: a shipped GUI
    // build has no console, and an unconditional one would format every line and
    // write it to a handle nothing can read. The file sink below is the log's real
    // destination there.
    if (Core::HasConsoleOutput())
    {
        Core::GetLogger().AddSink(std::make_shared<Core::ConsoleSink>());
    }
    // One file per launch, named for when the process started: a single truncated
    // log is destroyed by the relaunch after the crash it explains.
    const std::string logName = std::format("assisi-{}.log", Core::LaunchStamp());
    const std::filesystem::path logPath = Core::AssetSystem::ResolveUser(logName).value_or(logName);

    // Backstop retention, deliberately *before* this run's log exists — the one
    // file that must never be deleted cannot be, because there is nothing to
    // delete yet. game.json has not been read (it needs the asset system), so
    // this cannot use keepLogs; the cap is fixed and generous precisely so it
    // can never cut below anyone's configured value. InitializeCore prunes to
    // the real counts once they are known.
    //
    // Here as well as there because this runs on every launch whatever fails
    // afterwards: an install that fails asset init would otherwise write a log
    // every launch and prune none, growing without bound.
    const std::filesystem::path userRoot = logPath.parent_path();
    Core::PruneOldFiles(userRoot, "assisi-", ".log", kRetentionBackstop);
    Core::PruneOldFiles(userRoot, "crash-", CrashReportExtension(), kRetentionBackstop);

    Core::GetLogger().AddSink(std::make_shared<Core::FileSink>(logPath));

    // Named with the same launch stamp as the log, so a report and the log that
    // led up to it pair by name. Resolved now, before anything can crash: doing
    // filesystem resolution inside a handler, on a possibly-corrupt heap, is
    // exactly what the handler cannot afford.
    const std::string crashName = std::format("crash-{}{}", Core::LaunchStamp(), CrashReportExtension());
    InstallCrashHandlers(Core::AssetSystem::ResolveUser(crashName).value_or(crashName));
}

bool Application::Initialize()
{
    if (_initialized)
    {
        return true;
    }

    if (!InitializeCore())
    {
        return false;
    }

    // Headless stops here: no window is created, RenderSystem::Initialize is
    // never called, and nothing below this line touches a GPU.
    if (!_headless && !InitializePresentation())
    {
        return false;
    }

    _initialized = true;
    return true;
}

bool Application::InitializeCore()
{
    if (auto result = Core::AssetSystem::Initialize(); !result)
    {
        Core::Log::Fatal("Failed to initialize asset system.");
        return false;
    }

#ifdef ASSISI_SOURCE_ASSET_ROOT
    // Dev build: assets are read from the staged copy next to the executable
    // (generated .spv files exist only there), but that copy is disposable, so
    // any asset id minted into it alone is regenerated differently after a clean
    // build and every by-GUID reference to that asset silently stops resolving.
    // Mirror minted sidecars back into the source tree, which is the durable,
    // version-controlled copy. Not defined for Release — a shipped build has no
    // source tree, and its staged copy IS the durable one.
    Core::AssetSystem::SetAuthoringRoot(ASSISI_SOURCE_ASSET_ROOT);
#endif

    _config  = AppConfig::LoadFromJson();
    _options = OptionsConfig::LoadFromJson();

    // A capture must not be paced. Under vsync the frame time is the display's
    // refresh interval and the GPU idles between presents — which both hides the
    // renderer's real cost and lets the driver drop the core clock, so the
    // measurement is taken on hardware that is no longer at speed.
    if (_perfCapture)
    {
        _options.frameSync = FrameSyncMode::FpsLimit;
        _options.fpsLimit  = -1;
    }

    // Retention runs here rather than in the constructor because it is game.json
    // that says how many to keep. The counts are totals including this run.
    //
    // Both calls name this run's own artifact as protected. Relying on it
    // sorting newest is not enough: LaunchStamp() is local time, so a DST
    // fall-back or an NTP step backwards makes it sort oldest and it is deleted
    // first — the log while its descriptor is still open.
    //
    // Crash reports are pruned at startup, not at crash time — the crash handler
    // has no business enumerating a directory. This run's report does not exist
    // yet, so a run that does crash ends with keepDumps + 1 on disk until the
    // next launch trims it. Overshooting by one beats doing directory work with
    // a corrupt heap.
    const std::filesystem::path &userRoot   = Core::AssetSystem::GetUserRoot();
    const std::string logName    = std::format("assisi-{}.log", Core::LaunchStamp());
    const std::string crashName  = std::format("crash-{}{}", Core::LaunchStamp(), CrashReportExtension());
    Core::PruneOldFiles(userRoot, "assisi-", ".log", _config.keepLogs, logName);
    Core::PruneOldFiles(userRoot, "crash-", CrashReportExtension(), _config.keepDumps, crashName);

    // Either source turns it on; a --server flag must not be undone by a config
    // file that says nothing about headless mode.
    _headless = _headless || _config.headless;
    return true;
}

bool Application::InitializePresentation()
{
    // A capture's requested resolution wins over game.json — the ledger needs
    // both 1440p and 1080p from the same committed config.
    if (_captureWidth > 0 && _captureHeight > 0)
    {
        _config.width  = _captureWidth;
        _config.height = _captureHeight;
    }

    Window::WindowConfiguration winCfg;
    winCfg.Width  = _config.width;
    winCfg.Height = _config.height;
    winCfg.Title  = _config.title.c_str();
    // Undecorated for a capture, so the framebuffer is exactly the size asked
    // for: 1440p on a 1440p display does not fit once a title bar is added, and
    // a report labelled 1440p that rendered 2560x1400 is quoting a workload
    // nobody ran.
    winCfg.Undecorated = _perfCapture != nullptr;

    _window = std::make_unique<Window::WindowContext>(winCfg);
    if (!_window->IsValid())
    {
        Core::Log::Fatal("Failed to create window.");
        return false;
    }

    // Subscribe through WindowContext (the sole owner of GLFW callbacks) rather
    // than registering GLFW callbacks directly. WindowContext installed its
    // callbacks in its constructor, so ImGui (initialized below with
    // install_callbacks=true) chains to them instead of clobbering them.
    _window->OnFramebufferSize([this](int32_t width, int32_t height) { HandleFramebufferResize(width, height); });
    _window->OnWindowRefresh([this]() { RenderFrame(); });

    if (!Render::RenderSystem::Initialize(*_window))
    {
        Core::Log::Fatal("Failed to initialize render system.");
        return false;
    }

    Debug::DebugUI::Initialize(*_window, *Render::RenderSystem::GetVulkanContext(),
                               /*persistLayout=*/ !_restrictedViewer);

    _input = std::make_unique<Window::InputContext>(*_window);

    if (auto *vulkanContext = Render::RenderSystem::GetVulkanContext())
    {
        if (!_postProcess.Initialize({.device = vulkanContext->GetDevice(),
                                      .swapchainFramebufferInfo = vulkanContext->GetFramebufferInfo(),
                                      .vertexShaderSpvPath = "shaders/fullscreen.vert.spv",
                                      .tonemapShaderSpvPath = "shaders/tonemap.frag.spv",
                                      .fxaaShaderSpvPath = "shaders/fxaa.frag.spv"}))
        {
            Core::Log::Fatal("Failed to initialize post-process pipeline.");
            return false;
        }
        ConfigurePostProcess();

        // A capture is exactly the case the per-pass render-pass splits are
        // worth paying for: nobody is looking at this frame, and the whole point
        // of the run is to find out where the time went. Interactive runs leave
        // it to the F11 checkbox.
        if (_perfCapture && _capturePerPassTiming)
        {
            vulkanContext->SetPassTimingEnabled(true);
        }
    }

    _presentationInitialized = true;
    return true;
}

Window::WindowContext &Application::GetWindow() const
{
    ASSISI_ASSERT(_window != nullptr, "GetWindow() in a headless process — there is no window. Guard with "
                  "IsHeadless()/HasPresentation().");
    return *_window;
}

Window::InputContext &Application::GetInput() const
{
    ASSISI_ASSERT(_input != nullptr, "GetInput() in a headless process — there are no input devices. Guard with "
                  "IsHeadless()/HasPresentation().");
    return *_input;
}

Application::~Application()
{
    // DebugUI/render teardown only ran meaningful bring-up if the presentation
    // half was brought up — which a headless process skips entirely, while still
    // reporting a successful Initialize(). Tear the GPU stack down in order and
    // — crucially — here, before main() returns: the device lives in
    // RenderSystem's static, so leaving it to static destruction races the
    // dynamic loader that owns vulkan-1.dll (freeing the device through an
    // unloaded DLL is a crash). The GPU was already drained at the end of Run();
    // this only orders the releases: our own resources first, then the device last.
    if (_presentationInitialized)
    {
        Debug::DebugUI::Shutdown();
        _postProcess.Shutdown();
        Render::RenderSystem::Shutdown();
    }
}

void Application::RequestClose()
{
    _closeRequested = true;
    // Still forward it: the window is what a windowed process's OS-level close
    // path observes, and leaving the two out of sync would make an
    // externally-closed window and a programmatic close behave differently.
    if (_window)
    {
        _window->RequestClose();
    }
}

bool Application::ShouldClose() const
{
    return _closeRequested || (_window && _window->ShouldClose());
}

void Application::SetPerfCapture(const PerfCaptureConfig &config)
{
    _perfCapture = std::make_unique<PerfCapture>(config);

    // A capture under vsync measures the display's refresh, not the renderer, so
    // the pacing comes off here rather than being left to whatever options.json
    // happens to say. Written into _options so the rest of the loop and the
    // vsync reconcile above both see one answer.
    // Neither the pacing nor the resolution is applied here, and for the same
    // reason: Initialize() replaces _options from options.json and _config from
    // game.json, both of which run after this. Setting them now looks right and
    // is silently undone — which is exactly what happened, and is why every
    // early capture ran vsync-locked to the display and reported frame times
    // taken while the GPU sat idle between presents. They are applied after
    // those loads instead.
    _captureWidth         = config.width;
    _captureHeight        = config.height;
    _capturePerPassTiming = config.perPassTiming;
}

void Application::RecordCaptureFrame(double cpuMs, double gpuMs, double rawDt,
                                     Render::Vulkan::VulkanContext *context)
{
    PerfSample sample;
    sample.cpuMs        = cpuMs;
    sample.gpuMs        = gpuMs;
    sample.frameDeltaMs = rawDt * 1000.0;

    // Polled every frame rather than once: the clock guard's whole job is to
    // notice the hardware moving mid-run, and a single reading at either end
    // could not. Poll() returns the background worker's latest published sample
    // and never touches the driver, so this costs a copy.
    const Render::GpuTelemetrySample &telemetry = _captureTelemetry.Poll();
    sample.telemetryValid                       = telemetry.valid;
    sample.coreClockMhz                         = telemetry.coreClockMhz;
    sample.temperatureC                         = telemetry.temperatureC;
    sample.telemetrySequence                    = telemetry.sequence;

    _perfCapture->AddSample(sample);

    // After AddSample, which is what decides whether this frame counts.
    if (context != nullptr)
    {
        for (const Render::Vulkan::VulkanContext::PassTiming &pass : context->GetPassTimings())
        {
            _perfCapture->AddPassTiming(pass.name, static_cast<double>(pass.milliseconds));
        }
    }

    if (!_perfCapture->IsComplete())
    {
        return;
    }

    _perfCapture->SetDeviceName(telemetry.valid ? telemetry.name : std::string{});

    // The extent actually rendered, read off the framebuffer rather than the
    // size that was requested: a window manager is free to hand back something
    // else, and a report quoting the request would be quoting a resolution that
    // was never drawn.
    if (_window)
    {
        const Window::WindowSize size = _window->GetFramebufferSize();
        _perfCapture->SetRenderExtent(static_cast<uint32_t>(size.Width), static_cast<uint32_t>(size.Height));
    }
    Core::Log::Info("{}", _perfCapture->FormatReport());
    if (!_perfCapture->WriteReport())
    {
        Core::Log::Error("PerfCapture: the report could not be written.");
    }
    RequestClose();
}

namespace
{
using Clock = std::chrono::steady_clock;
/// Duration in fractional seconds — shared by the frame loop, SleepUntil, and the
/// slow-frame diagnostic, so the timing helpers agree on one representation.
using Seconds = std::chrono::duration<double>;

/// CPU frame time above which the main loop logs a per-phase breakdown (see the
/// slow-frame diagnostic in Run). Half a 60 Hz frame: high enough to stay quiet
/// in normal play, low enough that a streaming hitch is always reported.
constexpr double kSlowFrameMs = 8.0;

/// Waits until `target` while wasting as little CPU as possible. sleep_for()
/// overshoots its request by a scheduler-dependent amount, so we can't sleep the
/// whole way (we'd overshoot `target`) nor spin the whole way (that pins a core).
/// We sleep to `target` minus a small self-tuning margin, then busy-spin only the
/// sub-margin residual. The margin tracks the recently observed sleep overshoot:
/// it ratchets up fast when a sleep runs long and decays slowly otherwise, so it
/// stays just big enough to avoid overshooting without over-reserving spin time.
///
/// On Linux (hi-res timers) the real overshoot is ~60us, so the margin converges
/// there and the spin all but vanishes; on Windows (even with a 1ms timer period)
/// it settles nearer 1ms. Static (not thread_local) is fine — only the main loop
/// calls this.
void SleepUntil(Clock::time_point target)
{
    static double marginSec = 1e-3; // conservative seed; converges within a few frames

    const double remainingSec = Seconds(target - Clock::now()).count();
    if (remainingSec > marginSec)
    {
        const double requestSec = remainingSec - marginSec;
        const Clock::time_point before     = Clock::now();
        std::this_thread::sleep_for(Seconds(requestSec));
        const double overshootSec = std::max(0.0, Seconds(Clock::now() - before).count() - requestSec);

        // Ratchet up on a long sleep (with headroom), decay slowly otherwise;
        // clamp so one scheduling hiccup can't inflate the margin unboundedly and
        // so we never reserve more than a couple ms of spin.
        marginSec = std::clamp(std::max(overshootSec * 1.25, marginSec * 0.98), 50e-6, 3e-3);
    }

    while (Clock::now() < target)
    {
    } // spin the sub-margin residual (nothing at all if the sleep already reached target)
}
} // namespace

void Application::Run()
{
    if (!_initialized)
    {
        Core::Log::Error("Application::Run() called without a successful Initialize(); aborting.");
        return;
    }

#ifdef _WIN32
    TimerResolutionScope timerResolution;
#endif

    const double physicsStep = 1.0 / _config.physicsHz;

    OnStart();

    Clock::time_point prevTime       = Clock::now();
    Clock::time_point nextRenderTime = Clock::now();
    double accumulator    = 0.0;

    double fpsAccum       = 0.0;
    int32_t fpsFrameCount  = 0;
    double cpuMsAccum     = 0.0;
    double gpuMsAccum     = 0.0;

    // Headless pacing target: the next fixed tick. A windowed process paces on
    // frames (vsync or the FPS cap); a server has no frames, so without this it
    // would spin a core running an accumulator loop that mostly does nothing.
    Clock::time_point nextTickTime = Clock::now();

    while (!ShouldClose())
    {
        ASSISI_PROFILE_SCOPE("Frame");
        ASSISI_PROFILE_FRAME();

        // Frame pacing happens *first*, before input is polled and before the
        // frame clock starts.
        //
        // Two reasons, and the second is why it is here rather than just before
        // the render it paces. Input polled after the sleep is acted on
        // immediately, where sleeping mid-frame leaves every input up to a whole
        // pacing interval stale by the time it reaches the screen — 5 ms of the
        // 7 ms frame, on a 144 cap. And sitting above `now` puts the sleep outside
        // the measured window entirely, so cpuMs does not have to subtract it back
        // out; one less term to keep in sync with the rest of the loop.
        //
        // The cost is that render start jitters with however long input,
        // fixed-update and update took: ~0.16 ms against ~5 ms of latency saved.
        //
        // Pacing is exclusive with vsync: only cap in FpsLimit mode with a
        // finite limit. In VSync mode FIFO present paces us; with an unlimited
        // cap (fpsLimit < 0) we run as fast as the GPU allows.
        //
        // A headless process has no render to pace, so it paces the *tick*
        // instead — same placement, same reasoning, a different target.
        double sleepMs = 0.0;
        if (_headless)
        {
            ASSISI_PROFILE_SCOPE("pacing-sleep");

            // Pace to the next fixed tick with the same self-tuning sleep the
            // FPS cap uses. Advancing the target by exactly one step (rather
            // than from "now") keeps the tick rate honest across a long run: a
            // step that overruns is absorbed by the next one being shorter,
            // instead of every step drifting later by the overshoot.
            const Clock::time_point sleepStart = Clock::now();
            nextTickTime += std::chrono::duration_cast<Clock::duration>(Seconds(physicsStep));
            if (nextTickTime < sleepStart)
            {
                // Fell far enough behind that catching up tick-by-tick would
                // spiral. Give up the lost time rather than chase it.
                nextTickTime = sleepStart;
            }
            SleepUntil(nextTickTime);
            sleepMs = Seconds(Clock::now() - sleepStart).count() * 1000.0;
        }
        else if (_options.frameSync == FrameSyncMode::FpsLimit && _options.fpsLimit > 0)
        {
            ASSISI_PROFILE_SCOPE("pacing-sleep");
            const Clock::time_point sleepStart = Clock::now();
            SleepUntil(nextRenderTime);
            sleepMs = Seconds(Clock::now() - sleepStart).count() * 1000.0;

            // Advance the target rather than restarting it from now. SleepUntil
            // can only overshoot, so `= now + period` absorbs every overshoot
            // permanently and runs a hair under the cap forever — 6.999 ms against
            // a 6.944 ms period, 142.9 fps instead of 144. Accumulating the period
            // corrects the overshoot on the next frame instead.
            const Clock::duration period =
                std::chrono::duration_cast<Clock::duration>(Seconds(1.0 / _options.fpsLimit));
            nextRenderTime += period;

            // Unless we have fallen more than a frame behind — after a hitch or
            // a breakpoint, a target in the past would make the next several
            // frames skip their sleep entirely to "catch up", which is a burst
            // of unpaced frames rather than a recovery. Resnap instead.
            const Clock::time_point resumed = Clock::now();
            if (nextRenderTime < resumed)
            {
                nextRenderTime = resumed + period;
            }
        }

        const Clock::time_point now   = Clock::now();
        const double rawDt = Seconds(now - prevTime).count();
        const double dt    = std::min(rawDt, 0.25);
        prevTime                      = now;

        // Per-phase stopwatches for the slow-frame diagnostic below. Cheap
        // (steady_clock reads), and only reported when a frame actually spikes.
        const auto phaseMs = [](Clock::time_point from, Clock::time_point to)
                             { return Seconds(to - from).count() * 1000.0; };

        const Clock::time_point inputStart = Clock::now();
        {
            ASSISI_PROFILE_SCOPE("input");

            // A headless process has neither window nor input devices; the whole
            // phase is skipped rather than null-guarded per call, so the profile
            // slice reads as an empty phase instead of a fictitious one.
            if (!_headless)
            {
                Window::WindowContext::PollEvents();
                _input->Poll();
            }
        }
        const Clock::time_point inputEnd = Clock::now();

        {
            // One scope for the whole substep loop rather than one per substep:
            // what matters is the total the frame paid, and N nested identical
            // slices would bury it.
            ASSISI_PROFILE_SCOPE("fixed-update");
            accumulator += dt;
            while (accumulator >= physicsStep)
            {
                // The network clock. Incremented with the step, before the hook,
                // so OnFixedUpdate and every system it runs sees the tick they
                // are simulating rather than the one they just finished.
                ++_simTick;
                OnFixedUpdate(static_cast<float>(physicsStep));
                accumulator -= physicsStep;
            }
        }
        const Clock::time_point fixedEnd = Clock::now();

        // What's left in the accumulator is how far we are into the *next*
        // physics step — the blend factor OnRender uses to interpolate
        // physics-driven state between the last two fixed steps. The while loop
        // above guarantees accumulator < physicsStep, so this stays in [0, 1).
        _interpolationAlpha = static_cast<float>(accumulator / physicsStep);

        // Run work marshalled back to the main thread (Jobs().RunOnMain) at this
        // safe point — before OnUpdate's systems run and before any render command
        // list is open. This is where deferred level loads land (see SandboxApp);
        // background async results (streaming) publish here too. The budget (0 =
        // unbounded by default) lets an app spread a burst of streaming asset
        // publishes across frames — see SetMainThreadTaskBudget.
        {
            ASSISI_PROFILE_SCOPE("drain-main");
            _jobs.DrainMain(_mainThreadTaskBudget);

            // System installs queued by a blueprint spawn land here too, and for
            // the same reason the loads above do: spawning usually happens inside
            // a system, and SystemRegistry invalidates its cached execution order
            // on every registration — so registering mid-walk mutates what is
            // being iterated. Before OnUpdate, so a spawn made last frame is
            // running this one. Through the app, which is what owns the worlds the
            // queues live on.
            InstallQueuedSystems();
        }
        const Clock::time_point drainEnd = Clock::now();

        {
            ASSISI_PROFILE_SCOPE("update");
            OnUpdate(static_cast<float>(dt));
        }
        const Clock::time_point updateEnd = Clock::now();

        // Reconcile the swapchain's present mode with the frame-sync option HERE,
        // between frames — never inside RenderFrame(), which recreates the
        // swapchain mid command-list and destroys resources the frame still uses.
        // SetVSync() no-ops when already in the requested state, so this is a cheap
        // compare every frame and only recreates when the user actually changed it.
        // Applies the persisted option on the first iteration too.
        Render::Vulkan::VulkanContext *vulkanContext =
            _headless ? nullptr : Render::RenderSystem::GetVulkanContext();
        if (vulkanContext)
        {
            // Scoped so it cannot show as a blank gap between two slices. Normally
            // a compare and nothing else; when the user does flip the option it
            // rebuilds the swapchain, and a multi-millisecond stall with no slice
            // under it is exactly the kind of hole that sends you hunting in the
            // wrong place.
            ASSISI_PROFILE_SCOPE("vsync-reconcile");
            vulkanContext->SetVSync(_options.frameSync == FrameSyncMode::VSync);
        }

        // The brackets stay taken in headless too: renderMs then measures the
        // empty phase rather than going stale, so `unaccounted` below keeps
        // meaning the same thing on a server as it does in a window.
        const Clock::time_point renderStart = Clock::now();
        if (!_headless)
        {
            RenderFrame();
        }
        const Clock::time_point renderEnd = Clock::now();
        {
            ASSISI_PROFILE_SCOPE("flush");
            _events.Flush();
            FlushDeferred();
        }
        const Clock::time_point flushEnd = Clock::now();

        // Frame-time accounting. CPU frame time is this loop iteration's
        // wall-clock minus the one interval the CPU is deliberately idle inside
        // it: the frames-in-flight throttle where BeginFrame() blocks on the GPU
        // (reported by the context). What's left is the real CPU cost, so
        // comparing it against the GPU timer-query time shows which side is
        // bound. The pacing sleep happens above `now`, outside the window this
        // measures, so it needs no correction here.
        const double gpuWaitMs = vulkanContext ? vulkanContext->GetLastGpuWaitMs() : 0.0;
        const double cpuMs     = std::max(0.0, Seconds(Clock::now() - now).count() * 1000.0 - gpuWaitMs);
        const double gpuMs = vulkanContext ? static_cast<double>(vulkanContext->GetLastGpuFrameTimeMs()) : 0.0;

        // Slow-frame diagnostic. A spike is only actionable if you know which phase
        // ate it — and, crucially, whether ANY phase did. `unaccounted` is the gap
        // between the frame's measured CPU time and the sum of its phases: when the
        // phases are all small but the frame is long, the main thread was not doing
        // work, it was descheduled (the streaming/physics pools oversubscribing the
        // CPU). That distinction picks the fix, so it is reported explicitly.
        const double inputMs  = phaseMs(inputStart, inputEnd);
        const double fixedMs  = phaseMs(inputEnd, fixedEnd);
        const double drainMs  = phaseMs(fixedEnd, drainEnd);
        const double updateMs = phaseMs(drainEnd, updateEnd);
        const double renderMs = phaseMs(renderStart, renderEnd);
        const double flushMs  = phaseMs(renderEnd, flushEnd);

        // The render bracket contains the GPU wait (every accumulation site sits
        // inside RenderFrame's callees) but cpuMs already had it subtracted, so
        // summing the raw phases would bias `unaccounted` low by the whole wait.
        // Under VSync, where that wait is most of the frame, the figure would sit
        // pinned at zero and hide exactly the descheduling it exists to reveal.
        const double renderCpuMs = std::max(0.0, renderMs - gpuWaitMs);
        const double accounted   = inputMs + fixedMs + drainMs + updateMs + renderCpuMs + flushMs;

        // Deliberately not clamped. A persistently negative value means the
        // accounting itself is wrong — a phase double-counted, or a new one added
        // without a bracket — and that is worth seeing rather than flooring away.
        const double unaccountedMs = cpuMs - accounted;

        ASSISI_PROFILE_COUNTER("frame/cpu-ms", cpuMs);
        ASSISI_PROFILE_COUNTER("frame/gpu-ms", gpuMs);
        ASSISI_PROFILE_COUNTER("frame/gpu-wait-ms", gpuWaitMs);
        ASSISI_PROFILE_COUNTER("frame/sleep-ms", sleepMs);
        ASSISI_PROFILE_COUNTER("frame/unaccounted-ms", unaccountedMs);
        ASSISI_PROFILE_COUNTER("jobs/worker-queue-depth", static_cast<double>(_jobs.WorkerQueueDepth()));
        ASSISI_PROFILE_COUNTER("jobs/main-queue-depth", static_cast<double>(_jobs.MainQueueDepth()));
        PumpChiaraCounters();

        // A pointer to the capture rather than the breakdown itself: a spike you
        // can see in the log but not explain is worse than useless, and the
        // explanation is one dump away.
        if (cpuMs >= kSlowFrameMs)
        {
            Core::Log::Info("Slow frame {} — {:.2f} ms cpu (gpu {:.2f}, wait {:.2f}, unaccounted {:.2f}); "
                            "dump a capture for the breakdown",
                            Chiara::CurrentFrame(), cpuMs, gpuMs, gpuWaitMs, unaccountedMs);
        }

        // Record raw (un-averaged) per-frame samples for the plots so spikes stay
        // visible; the numeric readout uses the smoothed averages below. The full
        // frame delta (rawDt, including any vsync/pacing wait) drives the 1%-low
        // and min/max stats — that's the pacing the player actually feels.
        _cpuHistory[static_cast<std::size_t>(_frameHistoryOffset)]       = static_cast<float>(cpuMs);
        _gpuHistory[static_cast<std::size_t>(_frameHistoryOffset)]       = static_cast<float>(gpuMs);
        _frameTimeHistory[static_cast<std::size_t>(_frameHistoryOffset)] = static_cast<float>(rawDt * 1000.0);
        _frameHistoryOffset = (_frameHistoryOffset + 1) % kFrameHistory;
        if (_frameSampleCount < kFrameHistory)
        {
            ++_frameSampleCount;
        }

        if (_perfCapture)
        {
            RecordCaptureFrame(cpuMs, gpuMs, rawDt, vulkanContext);
        }

        fpsAccum += rawDt;
        cpuMsAccum += cpuMs;
        gpuMsAccum += gpuMs;
        ++fpsFrameCount;
        if (fpsAccum >= 0.5)
        {
            _fps          = static_cast<int32_t>(static_cast<double>(fpsFrameCount) / fpsAccum);
            _cpuFrameMs   = cpuMsAccum / fpsFrameCount;
            _gpuFrameMs   = gpuMsAccum / fpsFrameCount;
            fpsAccum      = 0.0;
            cpuMsAccum    = 0.0;
            gpuMsAccum    = 0.0;
            fpsFrameCount = 0;
        }
    }

    // Drain the GPU before any teardown begins. Render resources are owned all
    // over (OnShutdown below, then _postProcess/DebugUI in ~Application), but
    // the device itself lives in RenderSystem's static and isn't destroyed
    // until after main() returns — so without this, resources are freed while
    // the last frame's command buffer is still in flight (validation:
    // "destroy ... currently in use by VkCommandBuffer", then a crash).
    if (_presentationInitialized)
    {
        if (auto *vulkanContext = Render::RenderSystem::GetVulkanContext())
            vulkanContext->GetDevice()->waitForIdle();
    }

    OnShutdown();
}

void Application::PumpChiaraCounters()
{
    ASSISI_PROFILE_SCOPE("chiara-counters");

    // Resident set is a syscall, so it is sampled on a schedule rather than every
    // frame. A quarter-second cadence is far finer than any memory trend worth
    // seeing and stays invisible in the frame budget.
    static constexpr uint64_t kRssSampleInterval = 15;
    if (Chiara::CurrentFrame() % kRssSampleInterval == 0)
    {
        ASSISI_PROFILE_COUNTER("mem/process-rss-bytes", static_cast<double>(Core::ProcessResidentBytes()));

        // Render resolution, on the same schedule. GPU cost is dominated by
        // fragment work, so it scales with pixel count — which makes every
        // frame/gpu-ms number meaningless unless you know the size it was
        // measured at. A window the compositor sized differently between two
        // runs is invisible in the numbers and silently invalidates the
        // comparison; recording it makes a capture self-describing.
        if (_window != nullptr)
        {
            const Window::WindowSize fb = _window->GetFramebufferSize();
            ASSISI_PROFILE_COUNTER("render/framebuffer-width", static_cast<double>(fb.Width));
            ASSISI_PROFILE_COUNTER("render/framebuffer-height", static_cast<double>(fb.Height));
        }
    }

    // Streams a running session's events to disk before the rings can wrap over
    // them. Returns immediately unless one is running, and even then only does
    // real work when a buffer is filling — so the cost of asking every frame is
    // a couple of atomic loads.
    Chiara::PumpSession();

    // Physics allocation *churn* per frame, differenced from running totals —
    // Jolt's free hook takes no size, so residency is not knowable without a
    // header on every block. Churn is the perf-relevant signal anyway: a frame
    // that allocates is a frame that will pay to free.
    const Physics::JoltAllocationStats jolt = Physics::GetJoltAllocationStats();
    ASSISI_PROFILE_COUNTER("physics/alloc-count-per-frame",
                           static_cast<double>(jolt.count - _lastJoltAllocCount));
    ASSISI_PROFILE_COUNTER("physics/alloc-bytes-per-frame",
                           static_cast<double>(jolt.bytes - _lastJoltAllocBytes));
    _lastJoltAllocCount = jolt.count;
    _lastJoltAllocBytes = jolt.bytes;
}

void Application::RenderFrame()
{
    auto *vulkanContext = Render::RenderSystem::GetVulkanContext();
    if (!vulkanContext)
    {
        return;
    }

    ASSISI_PROFILE_SCOPE("render");

    std::optional<Render::RenderFrame> frame;
    {
        ASSISI_PROFILE_SCOPE("begin-frame");
        frame = vulkanContext->BeginFrame();
    }
    if (!frame.has_value())
    {
        return; // minimized, or swapchain is stale and about to be resized
    }

    // The scene always renders into PostProcess's HDR offscreen target — the
    // swapchain cannot hold radiance. Everything else about `frame` (commandList,
    // width, height) is unchanged.
    Render::RenderFrame sceneFrame = *frame;
    sceneFrame.framebuffer = _postProcess.SceneFramebuffer();
    sceneFrame.colorTexture = _postProcess.SceneColorTexture();
    sceneFrame.depthTexture = _postProcess.SceneDepthTexture();

    {
        // The last unscoped thing inside `render` — small, but an unnamed gap
        // between two slices is exactly what sends you looking in the wrong place.
        ASSISI_PROFILE_GPU_PASS(sceneFrame.commandList, "clear-targets");
        // The configured clear colour is an sRGB colour, and the scene target
        // holds radiance, so it is decoded on the way in — same treatment any
        // sRGB texture gets. The tone map puts it back where it was.
        const glm::vec3 clearLinear = glm::pow(glm::vec3(_config.clearColor), glm::vec3(2.2f));
        sceneFrame.commandList->clearTextureFloat(
            sceneFrame.colorTexture, nvrhi::AllSubresources,
            nvrhi::Color(clearLinear.r, clearLinear.g, clearLinear.b, _config.clearColor.a));
        if (sceneFrame.depthTexture)
        {
            sceneFrame.commandList->clearDepthStencilTexture(sceneFrame.depthTexture, nvrhi::AllSubresources, true,
                                                             1.0f, false, 0);
        }
    }

    {
        ASSISI_PROFILE_GPU_SCOPE(sceneFrame.commandList, "scene");
        OnRender(sceneFrame);
    }

    {
        // Resolve, then tone map: the chain's HDR half.
        ASSISI_PROFILE_GPU_PASS(frame->commandList, "post-process");
        _postProcess.RunBeforeOverlays(frame->commandList, *frame);
    }

    // Display-referred content, drawn into the tone-mapped image rather than
    // through the tone map. The target still carries the scene's depth, so
    // overlays occlude against the scene exactly as they did.
    if (nvrhi::IFramebuffer *overlayFramebuffer = _postProcess.OverlayFramebuffer())
    {
        Render::RenderFrame overlayFrame = *frame;
        overlayFrame.framebuffer = overlayFramebuffer;
        overlayFrame.depthTexture = _postProcess.SceneDepthTexture();

        // A scope, not a pass: the overlay passes open their own pass timers.
        ASSISI_PROFILE_GPU_SCOPE(overlayFrame.commandList, "overlays");
        OnRenderOverlays(overlayFrame);
    }

    {
        // Whatever is left: the overlay resolve, and FXAA or the final copy.
        ASSISI_PROFILE_GPU_PASS(frame->commandList, "post-process-output");
        _postProcess.RunAfterOverlays(frame->commandList, *frame);
    }

    {
        // Split three ways because the three costs move for unrelated reasons:
        // `imgui-begin` is the backend's per-frame setup plus the texture sweep,
        // `imgui-panels` is the app's own panel code (the part a game controls),
        // and `imgui-render` is building + recording the draw data, which scales
        // with how much got drawn rather than with how much code ran.
        ASSISI_PROFILE_GPU_PASS(frame->commandList, "imgui");
        {
            ASSISI_PROFILE_GPU_SCOPE(frame->commandList, "imgui-begin");
            Debug::DebugUI::BeginFrame(*frame);
        }
        {
            ASSISI_PROFILE_GPU_SCOPE(frame->commandList, "imgui-panels");
            OnImGui();
        }
        {
            ASSISI_PROFILE_GPU_SCOPE(frame->commandList, "imgui-render");
            Debug::DebugUI::EndFrame(*frame);
        }
    }

    {
        ASSISI_PROFILE_SCOPE("end-frame");
        vulkanContext->EndFrame();
    }
}

void Application::ConfigurePostProcess()
{
    const Window::WindowSize fb = _window->GetFramebufferSize();
    if (fb.Width <= 0 || fb.Height <= 0)
    {
        return;
    }

    // msaaSamples comes from user-editable JSON, which only whitelists {2,4,8} —
    // it says nothing about what this GPU can actually do. Requesting an
    // unsupported count fails texture creation, so clamp to the device limit.
    const auto *vulkanContext = Render::RenderSystem::GetVulkanContext();
    const uint32_t requestedSamples = static_cast<uint32_t>(_options.msaaSamples);
    const uint32_t maxSamples = vulkanContext != nullptr ? vulkanContext->GetMaxUsableSampleCount() : 1u;
    const uint32_t msaaSamples = std::min(requestedSamples, maxSamples);
    if (msaaSamples != requestedSamples)
    {
        Core::Log::Warn("MSAA: {}x requested but this device supports at most {}x for colour+depth targets; "
                        "using {}x.",
                        requestedSamples, maxSamples, msaaSamples);
    }

    const nvrhi::FramebufferInfo before = _postProcess.SceneFramebufferInfo();
    _postProcess.Configure(static_cast<uint32_t>(fb.Width), static_cast<uint32_t>(fb.Height), _options.aaMode,
                           msaaSamples, {.overlays = UsesOverlayStage()});
    const nvrhi::FramebufferInfo after = _postProcess.SceneFramebufferInfo();

    // Only fires for an actual sample-count change (F11 toggling into/out of
    // MSAA) — resizing alone never changes FramebufferInfo. When called during
    // Initialize() (before OnStart()), the derived OnRenderTargetsChanged runs
    // but no-ops because the derived render resources aren't built yet (e.g.
    // SandboxApp guards on MeshPass::IsValid()).
    if (!(before == after))
    {
        OnRenderTargetsChanged(after);
    }
}

} // namespace Assisi::App