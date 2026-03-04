#include <Assisi/Render/Backend/GraphicsBackend.hpp>
#include <Assisi/Render/RenderSystem.hpp>
#include <Assisi/Window/WindowContext.hpp>

#include <cstdlib>
#include <iostream>

int main()
{
    Assisi::Window::WindowConfiguration config;
    config.Width = 1280;
    config.Height = 720;
    config.Title = "Assisi Sandbox";
    config.EnableVSync = true;

    Assisi::Window::WindowContext window(config, nullptr);

    if (!window.IsValid())
    {
        std::cerr << "Failed to create window.\n";
        return EXIT_FAILURE;
    }

    if (!Assisi::Render::RenderSystem::Initialize(
            Assisi::Render::Backend::GraphicsBackend::OpenGL, window))
    {
        std::cerr << "Failed to initialize render system.\n";
        return EXIT_FAILURE;
    }

    while (!window.ShouldClose())
    {
        window.PollEvents();
        window.SwapBuffers();
    }

    return EXIT_SUCCESS;
}
