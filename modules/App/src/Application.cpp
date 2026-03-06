/// @file Application.cpp

// --- Platform timer (must come before other Windows headers) ----------------
#ifdef _WIN32
#    include <windows.h>
#    include <timeapi.h>
#    pragma comment(lib, "winmm.lib")
#endif

// --- OpenGL (must precede any GLFW include) ---------------------------------
#include <glad/glad.h>

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
#include <cstdlib>
#include <thread>

namespace Assisi::App
{

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

    _window = std::make_unique<Window::WindowContext>(winCfg, nullptr);
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

    Debug::DebugUI::Initialize(*_window);

    _input = std::make_unique<Window::InputContext>(*_window);
}

Application::~Application()
{
    Debug::DebugUI::Shutdown();
}

void Application::RequestClose()
{
    _window->RequestClose();
}

glm::mat4 Application::MakeProjection(float fovDegrees, float zNear, float zFar) const
{
    const float aspect = static_cast<float>(_config.width) / static_cast<float>(_config.height);
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

        glClearColor(_config.clearColor.r, _config.clearColor.g,
                     _config.clearColor.b, _config.clearColor.a);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        OnRender();

        Debug::DebugUI::BeginFrame();
        OnImGui();
        Debug::DebugUI::EndFrame();

        _window->SwapBuffers();
    }

    OnShutdown();
}

} // namespace Assisi::App