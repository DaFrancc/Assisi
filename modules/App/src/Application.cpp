/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file Application.cpp

// --- Platform timer (must come before other Windows headers) ----------------
#ifdef _WIN32
#    include <windows.h>
#    include <dbghelp.h>
#    include <timeapi.h>
#    pragma comment(lib, "dbghelp.lib")
#    pragma comment(lib, "winmm.lib")
#endif

// --- Engine headers ---------------------------------------------------------
#include <Assisi/App/Application.hpp>

#include <Assisi/Chiara/Profile.hpp>
#include <Assisi/Chiara/Serializer.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Sinks.hpp>
#include <Assisi/Core/Platform.hpp>
#include <Assisi/Debug/DebugUI.hpp>
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

#ifdef _WIN32

// Absolute crash-dump path, resolved under the writable user root at startup so
// the write target is fixed before a crash (doing filesystem resolution inside
// the handler, on a possibly-corrupt heap, is best avoided). Empty until the
// Application constructor sets it; the handler falls back to a bare name.
static std::string gCrashDumpPath;

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS *info)
{
    /* Write the minidump FIRST: Log::Fatal heap-allocates (std::format) and
       takes the logger mutex, either of which can deadlock or re-fault on a
       corrupted heap — losing the dump exactly when it matters. The dump path
       below is Win32-only, no CRT heap. */
    const char *dumpPath    = gCrashDumpPath.empty() ? "crash.dmp" : gCrashDumpPath.c_str();
    bool        dumpWritten = false;
    HANDLE hFile = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers    = FALSE;
        dumpWritten = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpNormal, &mei,
                                        nullptr, nullptr) != FALSE;
        CloseHandle(hFile);
    }

    const DWORD code = info->ExceptionRecord->ExceptionCode;

    const char *name = "UNKNOWN";
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:    name = "ACCESS_VIOLATION";    break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: name = "ILLEGAL_INSTRUCTION"; break;
    case EXCEPTION_STACK_OVERFLOW:      name = "STACK_OVERFLOW";      break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:  name = "INT_DIVIDE_BY_ZERO";  break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:  name = "FLT_DIVIDE_BY_ZERO";  break;
    case EXCEPTION_IN_PAGE_ERROR:       name = "IN_PAGE_ERROR";       break;
    default:                            break;
    }

    Assisi::Core::Log::Fatal("Crash: unhandled exception 0x{:08X} ({})", static_cast<uint32_t>(code), name);
    if (dumpWritten)
    {
        Assisi::Core::Log::Fatal("Crash: minidump written to {}", dumpPath);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

static void AbortHandler(int)
{
    Assisi::Core::Log::Fatal("Crash: abort() called (assertion failure or std::terminate).");
}

#endif // _WIN32

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

Application::Application()
{
    // Only infallible setup belongs in the constructor. Logging is wired up
    // first so that any log lines emitted by derived-class member constructors
    // (which run after this base ctor) are captured. All fallible engine
    // bring-up lives in Initialize() so failures can unwind normally instead
    // of std::exit()-ing past every destructor.
    // Log and crash dump are writable runtime outputs — resolve them under the
    // user root (defaults to the exe dir) so they don't depend on the CWD the
    // process was launched from. The user root initializes lazily here, before
    // AssetSystem::Initialize() discovers the read-only asset root.
    // Only when there is somewhere for it to go. A shipped GUI build has no
    // console, and an unconditional ConsoleSink would format every line and
    // write it to a handle nothing can read — the log's real destination in
    // that build is the file sink below.
    if (Core::HasConsoleOutput())
    {
        Core::GetLogger().AddSink(std::make_shared<Core::ConsoleSink>());
    }
    const std::filesystem::path logPath = Core::AssetSystem::ResolveUser("assisi.log").value_or("assisi.log");
    Core::GetLogger().AddSink(std::make_shared<Core::FileSink>(logPath));

#ifdef _WIN32
    const std::expected<std::filesystem::path, Core::AssetError> dumpPath = Core::AssetSystem::ResolveUser("crash.dmp");
    if (dumpPath)
    {
        gCrashDumpPath = dumpPath->string();
    }
    SetUnhandledExceptionFilter(CrashHandler);
    std::signal(SIGABRT, AbortHandler);
#endif
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

    // Either source turns it on; a --server flag must not be undone by a config
    // file that says nothing about headless mode.
    _headless = _headless || _config.headless;
    return true;
}

bool Application::InitializePresentation()
{
    Window::WindowConfiguration winCfg;
    winCfg.Width  = _config.width;
    winCfg.Height = _config.height;
    winCfg.Title  = _config.title.c_str();

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
                               /*persistLayout=*/!_restrictedViewer);

    _input = std::make_unique<Window::InputContext>(*_window);

    if (auto *vulkanContext = Render::RenderSystem::GetVulkanContext())
    {
        if (!_postProcess.Initialize(vulkanContext->GetDevice(), vulkanContext->GetFramebufferInfo(),
                                     "shaders/fullscreen.vert.spv", "shaders/fxaa.frag.spv"))
        {
            Core::Log::Fatal("Failed to initialize post-process pipeline.");
            return false;
        }
        ConfigurePostProcess();
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
/// The old version reserved a fixed 2ms spin margin, so every capped frame
/// busy-waited up to 2ms. On Linux (hi-res timers) the real overshoot is ~60us,
/// so the margin converges there and the spin all but vanishes; on Windows (even
/// with a 1ms timer period) it settles nearer 1ms. Static (not thread_local) is
/// fine — only the main loop calls this.
void SleepUntil(Clock::time_point target)
{
    static double marginSec = 1e-3; // conservative seed; converges within a few frames

    const double remainingSec = Seconds(target - Clock::now()).count();
    if (remainingSec > marginSec)
    {
        const double           requestSec = remainingSec - marginSec;
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

    // Clock and Seconds are both aliased at file scope; the local re-declaration
    // that used to sit here shadowed the outer one and said in a comment that it
    // did not.
    const double physicsStep = 1.0 / _config.physicsHz;

    OnStart();

    Clock::time_point prevTime       = Clock::now();
    Clock::time_point nextRenderTime = Clock::now();
    double            accumulator    = 0.0;

    double  fpsAccum       = 0.0;
    int32_t fpsFrameCount  = 0;
    double  cpuMsAccum     = 0.0;
    double  gpuMsAccum     = 0.0;

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
        // immediately, where sleeping mid-frame left every input up to a whole
        // pacing interval stale by the time it reached the screen — 5 ms of the
        // 7 ms frame, on a 144 cap. And sitting above `now` puts the sleep
        // outside the measured window entirely, so cpuMs no longer has to
        // subtract it back out; one less term to keep in sync with the code,
        // which is precisely how the unaccounted figure went wrong before.
        //
        // The cost is that render start now jitters with however long input,
        // fixed-update and update took. That is ~0.16 ms against ~5 ms of
        // latency saved, so it is not a close call.
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
            // can only overshoot, so `= now + period` absorbed every overshoot
            // permanently and the loop ran a hair under the cap forever — a
            // measured 6.999 ms against a 6.944 ms period, 142.9 fps instead of
            // 144. Accumulating the period corrects the overshoot on the next
            // frame instead.
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
        const double            rawDt = Seconds(now - prevTime).count();
        const double            dt    = std::min(rawDt, 0.25);
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
            // Scoped because it is the other thing that used to sit between two
            // scopes and show as a blank gap. Normally a compare and nothing
            // else; when the user does flip the option it rebuilds the swapchain,
            // and a multi-millisecond stall with no slice under it is exactly
            // the kind of hole that sends you hunting in the wrong place.
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
        // bound.
        //
        // The pacing sleep used to need subtracting here too. It now happens
        // above `now`, outside the window this measures, so there is nothing to
        // correct for — the term that has to agree with code elsewhere in the
        // loop is simply gone.
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

        // The breakdown that used to be spelled out here is a capture now. What
        // survives is the pointer to it: a spike you can see in the log but not
        // explain is worse than useless, and the whole point of Chiara is that
        // the explanation is one dump away.
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

    // Physics allocation *churn* per frame, differenced from running totals —
    // Jolt's free hook takes no size, so residency is not knowable without a
    // header on every block. Churn is the perf-relevant signal anyway: a frame
    // that allocates is a frame that will pay to free.
    // Streams a running session's events to disk before the rings can wrap over
    // them. Returns immediately unless one is running, and even then only does
    // real work when a buffer is filling — so the cost of asking every frame is
    // a couple of atomic loads.
    Chiara::PumpSession();

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

    // When an AA mode is active, the scene renders into PostProcess's offscreen
    // target instead of the swapchain directly — everything else about `frame`
    // (commandList, width, height) stays the same either way.
    Render::RenderFrame sceneFrame = *frame;
    if (nvrhi::IFramebuffer *offscreenFramebuffer = _postProcess.SceneFramebuffer())
    {
        sceneFrame.framebuffer = offscreenFramebuffer;
        sceneFrame.colorTexture = _postProcess.SceneColorTexture();
        sceneFrame.depthTexture = _postProcess.SceneDepthTexture();
    }

    {
        // The last unscoped thing inside `render` — small, but an unnamed gap
        // between two slices is exactly what sends you looking in the wrong place.
        ASSISI_PROFILE_GPU_SCOPE(sceneFrame.commandList, "clear-targets");
        sceneFrame.commandList->clearTextureFloat(
            sceneFrame.colorTexture, nvrhi::AllSubresources,
            nvrhi::Color(_config.clearColor.r, _config.clearColor.g, _config.clearColor.b, _config.clearColor.a));
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
        // No-op if AA is off (the scene already rendered directly into `frame`
        // above); otherwise resolves/FXAA's the offscreen render into it.
        ASSISI_PROFILE_GPU_SCOPE(frame->commandList, "post-process");
        _postProcess.Resolve(frame->commandList, *frame);
    }

    {
        // Split three ways because the three costs move for unrelated reasons:
        // `imgui-begin` is the backend's per-frame setup plus the texture sweep,
        // `imgui-panels` is the app's own panel code (the part a game controls),
        // and `imgui-render` is building + recording the draw data, which scales
        // with how much got drawn rather than with how much code ran.
        ASSISI_PROFILE_GPU_SCOPE(frame->commandList, "imgui");
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
                           msaaSamples);
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