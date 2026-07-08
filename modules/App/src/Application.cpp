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

#include <GLFW/glfw3.h>

// --- Engine headers ---------------------------------------------------------
#include <Assisi/App/Application.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Sinks.hpp>
#include <Assisi/Debug/DebugUI.hpp>
#include <Assisi/Render/RenderSystem.hpp>
#include <Assisi/Window/Key.hpp>

#include <imgui.h>

// --- Standard ---------------------------------------------------------------
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <thread>

#ifdef _WIN32

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS *info)
{
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

    HANDLE hFile = CreateFileA("crash.dmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers    = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(hFile);
        Assisi::Core::Log::Fatal("Crash: minidump written to crash.dmp");
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

// ImGui's GLFW backend also uses glfwSetWindowUserPointer, which would overwrite
// an Application* stored there. Use a plain static instead.
static Application *s_instance = nullptr;

void Application::FramebufferSizeCallback(Window::NativeWindowHandle * /*window*/, int width, int height)
{
    // GLFW can fire this during window creation/show (e.g. a DPI-driven WM_SIZE on
    // Windows) — before s_instance is assigned and before RenderSystem::Initialize
    // has run. Bail out entirely rather than falling through to a Vulkan call
    // against a backend that isn't set up yet.
    if (width <= 0 || height <= 0 || s_instance == nullptr)
    {
        return;
    }

    if (auto *vulkanContext = Render::RenderSystem::GetVulkanContext())
    {
        vulkanContext->Resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }

    s_instance->OnResize(width, height);
    s_instance->ConfigurePostProcess();
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
    Core::GetLogger().AddSink(std::make_shared<Core::ConsoleSink>());
    Core::GetLogger().AddSink(std::make_shared<Core::FileSink>("assisi.log"));

#ifdef _WIN32
    SetUnhandledExceptionFilter(CrashHandler);
    std::signal(SIGABRT, AbortHandler);
#endif

    if (auto result = Core::AssetSystem::Initialize(); !result)
    {
        Core::Log::Fatal("Failed to initialize asset system.");
        std::exit(EXIT_FAILURE);
    }

    _config = AppConfig::LoadFromJson();

    Window::WindowConfiguration winCfg;
    winCfg.Width  = _config.width;
    winCfg.Height = _config.height;
    winCfg.Title  = _config.title.c_str();

    _window = std::make_unique<Window::WindowContext>(winCfg, FramebufferSizeCallback);
    if (!_window->IsValid())
    {
        Core::Log::Fatal("Failed to create window.");
        std::exit(EXIT_FAILURE);
    }

    if (!Render::RenderSystem::Initialize(*_window))
    {
        Core::Log::Fatal("Failed to initialize render system.");
        std::exit(EXIT_FAILURE);
    }

    Debug::DebugUI::Initialize(*_window, *Render::RenderSystem::GetVulkanContext());

    s_instance = this;

    glfwSetWindowRefreshCallback(_window->NativeHandle(), WindowRefreshCallback);

    _input = std::make_unique<Window::InputContext>(*_window);
    _options = OptionsConfig::LoadFromJson();

    if (auto *vulkanContext = Render::RenderSystem::GetVulkanContext())
    {
        _postProcess.Initialize(vulkanContext->GetDevice(), vulkanContext->GetFramebufferInfo(),
                                "shaders/fullscreen.vert.spv", "shaders/fxaa.frag.spv");
        ConfigurePostProcess();
    }
}

Application::~Application()
{
    s_instance = nullptr;
    Debug::DebugUI::Shutdown();
}

void Application::RequestClose()
{
    _window->RequestClose();
}

namespace
{
using Clock = std::chrono::steady_clock;

/// Waits until `target`. Sleeps for most of the remaining time, leaving
/// `margin` to spin — sleep_for() is coarse/jittery, but a short busy-wait
/// for the last couple of ms reliably lands on the target instead of
/// overshooting it.
void SleepUntil(Clock::time_point target, Clock::duration margin = std::chrono::milliseconds(2))
{
    const Clock::duration remaining = target - Clock::now();
    if (remaining <= Clock::duration::zero())
    {
        return; // already at/past the target — don't wait at all
    }

    if (remaining > margin)
    {
        std::this_thread::sleep_for(remaining - margin);
    }

    while (Clock::now() < target)
    {
    } // spin for the remainder (all of it, if remaining <= margin)
}
} // namespace

void Application::Run()
{
#ifdef _WIN32
    TimerResolutionScope timerResolution;
#endif

    using Clock   = std::chrono::steady_clock;
    using Seconds = std::chrono::duration<double>;

    const double physicsStep = 1.0 / _config.physicsHz;
    const double renderStep  = 1.0 / _config.renderHz;

    _window->SetVSyncEnabled(false);

    OnStart();

    Clock::time_point prevTime       = Clock::now();
    Clock::time_point nextRenderTime = Clock::now();
    double            accumulator    = 0.0;

    double fpsAccum       = 0.0;
    int    fpsFrameCount  = 0;

    while (!_window->ShouldClose())
    {
        const Clock::time_point now   = Clock::now();
        const double            rawDt = Seconds(now - prevTime).count();
        const double            dt    = std::min(rawDt, 0.25);
        prevTime                      = now;

        Window::WindowContext::PollEvents();
        _input->Poll();

        if (_input->IsKeyPressed(Window::Key::F12))
        {
            _showOptionsWindow = !_showOptionsWindow;
        }

        accumulator += dt;
        while (accumulator >= physicsStep)
        {
            OnFixedUpdate(static_cast<float>(physicsStep));
            accumulator -= physicsStep;
        }

        OnUpdate(static_cast<float>(dt));

        SleepUntil(nextRenderTime);
        nextRenderTime = Clock::now() + std::chrono::duration_cast<Clock::duration>(Seconds(renderStep));

        // FPS tracking.
        fpsAccum += rawDt;
        ++fpsFrameCount;
        if (fpsAccum >= 0.5)
        {
            _fps          = static_cast<int>(static_cast<double>(fpsFrameCount) / fpsAccum);
            fpsAccum      = 0.0;
            fpsFrameCount = 0;
        }

        RenderFrame();
        Core::EventQueue::Instance().Flush();
    }

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
    Render::Vulkan::VulkanFrame sceneFrame = *frame;
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
    DrawOptionsWindow();

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

    // Only fires for an actual sample-count change (F12 toggling into/out of
    // MSAA) — resizing alone never changes FramebufferInfo. During this
    // Application's own constructor (before the derived class exists), the
    // virtual call below harmlessly resolves to the no-op base implementation.
    if (!(before == after))
    {
        OnRenderTargetsChanged(after);
    }
}

void Application::DrawOptionsWindow()
{
    if (!_showOptionsWindow)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(300, 110), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Options", &_showOptionsWindow))
    {
        static const char *kModeNames[] = {"Disabled", "MSAA", "FXAA", "MSAA + FXAA"};
        int                modeIndex    = static_cast<int>(_options.aaMode);
        if (ImGui::Combo("AA Mode", &modeIndex, kModeNames, 4))
        {
            _options.aaMode = static_cast<Render::AaMode>(modeIndex);
            ConfigurePostProcess();
            _options.SaveToJson();
        }

        const bool msaaActive =
            (_options.aaMode == Render::AaMode::MSAA || _options.aaMode == Render::AaMode::MSAA_FXAA);
        if (!msaaActive)
        {
            ImGui::BeginDisabled();
        }

        static const char *kSampleNames[]  = {"2x", "4x", "8x"};
        static const int   kSampleValues[] = {2, 4, 8};
        int                sampleIndex     = 1;
        for (int i = 0; i < 3; ++i)
        {
            if (kSampleValues[i] == _options.msaaSamples)
            {
                sampleIndex = i;
                break;
            }
        }

        if (ImGui::Combo("MSAA Samples", &sampleIndex, kSampleNames, 3))
        {
            _options.msaaSamples = kSampleValues[sampleIndex];
            if (msaaActive)
            {
                ConfigurePostProcess();
            }
            _options.SaveToJson();
        }

        if (!msaaActive)
        {
            ImGui::EndDisabled();
        }
    }
    ImGui::End();
}

void Application::WindowRefreshCallback(Window::NativeWindowHandle * /*window*/)
{
    if (s_instance)
    {
        s_instance->RenderFrame();
    }
}

} // namespace Assisi::App