/// @file Application.cpp

// --- Platform timer (must come before other Windows headers) ----------------
#ifdef _WIN32
#    include <windows.h>
#    include <dbghelp.h>
#    include <timeapi.h>
#    pragma comment(lib, "dbghelp.lib")
#    pragma comment(lib, "winmm.lib")
#endif

// --- OpenGL (must precede any GLFW include) ---------------------------------
#include <glad/glad.h>
#include <GLFW/glfw3.h>

// --- Engine headers ---------------------------------------------------------
#include <Assisi/App/Application.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Sinks.hpp>
#include <Assisi/Debug/DebugUI.hpp>
#include <Assisi/Render/Backend/GraphicsBackend.hpp>
#include <Assisi/Render/RenderSystem.hpp>

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
    if (width <= 0 || height <= 0)
    {
        return; // minimized — nothing to do
    }

    glViewport(0, 0, width, height);
    if (s_instance)
    {
        s_instance->OnResize(width, height);
    }
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

    if (!Render::RenderSystem::Initialize(Render::Backend::GraphicsBackend::OpenGL, *_window))
    {
        Core::Log::Fatal("Failed to initialize render system.");
        std::exit(EXIT_FAILURE);
    }

    glEnable(GL_DEPTH_TEST);

    s_instance = this;

    glfwSetWindowRefreshCallback(_window->NativeHandle(), WindowRefreshCallback);

    Debug::DebugUI::Initialize(*_window);

    _input = std::make_unique<Window::InputContext>(*_window);
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

glm::mat4 Application::MakeProjection(float fovDegrees, float zNear, float zFar) const
{
    const Window::WindowSize fb = _window->GetFramebufferSize();
    if (fb.Width <= 0 || fb.Height <= 0)
    {
        return glm::mat4(1.f);
    }
    const float aspect = static_cast<float>(fb.Width) / static_cast<float>(fb.Height);
    return glm::perspective(glm::radians(fovDegrees), aspect, zNear, zFar);
}

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

    // Measure actual sleep resolution after timeBeginPeriod(1) is in effect.
    _sleepResolutionMs = [&]() -> double
    {
        constexpr int kSamples = 20;
        double total = 0.0;
        for (int i = 0; i < kSamples; ++i)
        {
            const auto before = Clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            total += Seconds(Clock::now() - before).count();
        }
        return (total / kSamples) * 1000.0;
    }();

    OnStart();

    auto   prevTime       = Clock::now();
    auto   nextRenderTime = Clock::now();
    double accumulator    = 0.0;

    auto   renderPrevTime = Clock::now();
    double fpsAccum       = 0.0;
    int    fpsFrameCount  = 0;

    while (!_window->ShouldClose())
    {
        const auto   now = Clock::now();
        const double dt  = std::min(Seconds(now - prevTime).count(), 0.25);
        prevTime         = now;

        Window::WindowContext::PollEvents();
        _input->Poll();

        accumulator += dt;
        while (accumulator >= physicsStep)
        {
            OnFixedUpdate(static_cast<float>(physicsStep));
            accumulator -= physicsStep;
        }

        OnUpdate(static_cast<float>(dt));

        // Hybrid sleep: yield to the OS in 1 ms chunks, spin the last millisecond.
        {
            constexpr auto kSpinThreshold = std::chrono::milliseconds(1);
            while (Clock::now() < nextRenderTime - kSpinThreshold)
            {
                std::this_thread::sleep_for(kSpinThreshold);
            }
            while (Clock::now() < nextRenderTime)
            {
            }
        }
        nextRenderTime += std::chrono::duration_cast<Clock::duration>(Seconds(renderStep));

        // FPS tracking.
        {
            const auto renderNow = Clock::now();
            fpsAccum += Seconds(renderNow - renderPrevTime).count();
            renderPrevTime = renderNow;
            ++fpsFrameCount;
            if (fpsAccum >= 0.5)
            {
                _fps          = static_cast<int>(static_cast<double>(fpsFrameCount) / fpsAccum);
                fpsAccum      = 0.0;
                fpsFrameCount = 0;
            }
        }

        RenderFrame();
    }

    OnShutdown();
}

void Application::RenderFrame()
{
    glClearColor(_config.clearColor.r, _config.clearColor.g,
                 _config.clearColor.b, _config.clearColor.a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    OnRender();

    Debug::DebugUI::BeginFrame();
    OnImGui();
    Debug::DebugUI::EndFrame();

    _window->SwapBuffers();
}

void Application::WindowRefreshCallback(Window::NativeWindowHandle * /*window*/)
{
    if (s_instance)
    {
        s_instance->RenderFrame();
    }
}

} // namespace Assisi::App