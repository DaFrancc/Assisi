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
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Sinks.hpp>
#include <Assisi/Debug/DebugUI.hpp>
#include <Assisi/Render/RenderSystem.hpp>

// --- Standard ---------------------------------------------------------------
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
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

    Assisi::Core::Log::Fatal("Crash: unhandled exception 0x{:08X} ({})", static_cast<unsigned int>(code), name);
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

void Application::HandleFramebufferResize(int width, int height)
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
    Core::GetLogger().AddSink(std::make_shared<Core::ConsoleSink>());
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

    _config = AppConfig::LoadFromJson();

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
    _window->OnFramebufferSize([this](int width, int height) { HandleFramebufferResize(width, height); });
    _window->OnWindowRefresh([this]() { RenderFrame(); });

    if (!Render::RenderSystem::Initialize(*_window))
    {
        Core::Log::Fatal("Failed to initialize render system.");
        return false;
    }

    Debug::DebugUI::Initialize(*_window, *Render::RenderSystem::GetVulkanContext());

    _input = std::make_unique<Window::InputContext>(*_window);
    _options = OptionsConfig::LoadFromJson();

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

    _initialized = true;
    return true;
}

Application::~Application()
{
    // DebugUI/render teardown only ran meaningful bring-up if Initialize()
    // succeeded. Tear the GPU stack down in order and — crucially — here,
    // before main() returns: the device lives in RenderSystem's static, so
    // leaving it to static destruction races the dynamic loader that owns
    // vulkan-1.dll (freeing the device through an unloaded DLL is a crash).
    // The GPU was already drained at the end of Run(); this only orders the
    // releases: our own resources first, then the device last.
    if (_initialized)
    {
        Debug::DebugUI::Shutdown();
        _postProcess.Shutdown();
        Render::RenderSystem::Shutdown();
    }
}

void Application::RequestClose()
{
    _window->RequestClose();
}

namespace
{
using Clock = std::chrono::steady_clock;

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
    using Seconds = std::chrono::duration<double>;

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

    using Clock   = std::chrono::steady_clock;
    using Seconds = std::chrono::duration<double>;

    const double physicsStep = 1.0 / _config.physicsHz;

    OnStart();

    Clock::time_point prevTime       = Clock::now();
    Clock::time_point nextRenderTime = Clock::now();
    double            accumulator    = 0.0;

    double fpsAccum       = 0.0;
    int    fpsFrameCount  = 0;
    double cpuMsAccum     = 0.0;
    double gpuMsAccum     = 0.0;

    while (!_window->ShouldClose())
    {
        const Clock::time_point now   = Clock::now();
        const double            rawDt = Seconds(now - prevTime).count();
        const double            dt    = std::min(rawDt, 0.25);
        prevTime                      = now;

        Window::WindowContext::PollEvents();
        _input->Poll();

        accumulator += dt;
        while (accumulator >= physicsStep)
        {
            OnFixedUpdate(static_cast<float>(physicsStep));
            accumulator -= physicsStep;
        }

        // What's left in the accumulator is how far we are into the *next*
        // physics step — the blend factor OnRender uses to interpolate
        // physics-driven state between the last two fixed steps. The while loop
        // above guarantees accumulator < physicsStep, so this stays in [0, 1).
        _interpolationAlpha = static_cast<float>(accumulator / physicsStep);

        // Run work marshalled back to the main thread (Jobs().RunOnMain) at this
        // safe point — before OnUpdate's systems run and before any render command
        // list is open. This is where deferred level loads land (see SandboxApp);
        // background async results (streaming) will publish here too.
        _jobs.DrainMain();

        OnUpdate(static_cast<float>(dt));

        // Frame pacing is exclusive with vsync: only cap here in FpsLimit mode with
        // a finite limit. In VSync mode FIFO present paces us; with an unlimited cap
        // (fpsLimit < 0) we run as fast as the GPU allows. The sleep is timed so it
        // can be excluded from the CPU frame-time figure below.
        double sleepMs = 0.0;
        if (_options.frameSync == FrameSyncMode::FpsLimit && _options.fpsLimit > 0)
        {
            const Clock::time_point sleepStart = Clock::now();
            SleepUntil(nextRenderTime);
            sleepMs = Seconds(Clock::now() - sleepStart).count() * 1000.0;
            nextRenderTime =
                Clock::now() + std::chrono::duration_cast<Clock::duration>(Seconds(1.0 / _options.fpsLimit));
        }

        // Reconcile the swapchain's present mode with the frame-sync option HERE,
        // between frames — never inside RenderFrame(), which recreates the
        // swapchain mid command-list and destroys resources the frame still uses.
        // SetVSync() no-ops when already in the requested state, so this is a cheap
        // compare every frame and only recreates when the user actually changed it.
        // Applies the persisted option on the first iteration too.
        Render::Vulkan::VulkanContext *vulkanContext = Render::RenderSystem::GetVulkanContext();
        if (vulkanContext)
        {
            vulkanContext->SetVSync(_options.frameSync == FrameSyncMode::VSync);
        }

        RenderFrame();
        _events.Flush();
        FlushDeferred();

        // Frame-time accounting. CPU frame time is this loop iteration's wall-clock
        // minus the two intervals the CPU is deliberately idle: the FPS-limit sleep
        // above, and the frames-in-flight throttle where BeginFrame() blocks on the
        // GPU (reported by the context). What's left is the real CPU cost, so
        // comparing it against the GPU timer-query time shows which side is bound.
        const double gpuWaitMs = vulkanContext ? vulkanContext->GetLastGpuWaitMs() : 0.0;
        const double cpuMs =
            std::max(0.0, Seconds(Clock::now() - now).count() * 1000.0 - sleepMs - gpuWaitMs);
        const double gpuMs = vulkanContext ? vulkanContext->GetLastGpuFrameTimeMs() : 0.0;

        // Record raw (un-averaged) per-frame samples for the plots so spikes stay
        // visible; the numeric readout uses the smoothed averages below. The full
        // frame delta (rawDt, including any vsync/pacing wait) drives the 1%-low
        // and min/max stats — that's the pacing the player actually feels.
        _cpuHistory[_frameHistoryOffset]       = static_cast<float>(cpuMs);
        _gpuHistory[_frameHistoryOffset]       = static_cast<float>(gpuMs);
        _frameTimeHistory[_frameHistoryOffset] = static_cast<float>(rawDt * 1000.0);
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
            _fps          = static_cast<int>(static_cast<double>(fpsFrameCount) / fpsAccum);
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
    if (auto *vulkanContext = Render::RenderSystem::GetVulkanContext())
        vulkanContext->GetDevice()->waitForIdle();

    OnShutdown();
}

void Application::RenderFrame()
{
    auto *vulkanContext = Render::RenderSystem::GetVulkanContext();
    if (!vulkanContext)
    {
        return;
    }

    auto frame = vulkanContext->BeginFrame();
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

    sceneFrame.commandList->clearTextureFloat(
        sceneFrame.colorTexture, nvrhi::AllSubresources,
        nvrhi::Color(_config.clearColor.r, _config.clearColor.g, _config.clearColor.b, _config.clearColor.a));
    if (sceneFrame.depthTexture)
    {
        sceneFrame.commandList->clearDepthStencilTexture(sceneFrame.depthTexture, nvrhi::AllSubresources, true, 1.0f,
                                                          false, 0);
    }

    OnRender(sceneFrame);

    // No-op if AA is off (the scene already rendered directly into `frame`
    // above); otherwise resolves/FXAA's the offscreen render into it.
    _postProcess.Resolve(frame->commandList, *frame);

    Debug::DebugUI::BeginFrame(*frame);

    OnImGui();

    Debug::DebugUI::EndFrame(*frame);

    vulkanContext->EndFrame();
}

void Application::ConfigurePostProcess()
{
    const Window::WindowSize fb = _window->GetFramebufferSize();
    if (fb.Width <= 0 || fb.Height <= 0)
    {
        return;
    }

    const nvrhi::FramebufferInfo before = _postProcess.SceneFramebufferInfo();
    _postProcess.Configure(static_cast<uint32_t>(fb.Width), static_cast<uint32_t>(fb.Height), _options.aaMode,
                           static_cast<uint32_t>(_options.msaaSamples));
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