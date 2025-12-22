#pragma once

#ifndef RENDERSYSTEM_HPP
#define RENDERSYSTEM_HPP

#include <iostream>

#include <Assisi/Render/Backend/GraphicsBackend.hpp>
#include <Assisi/Window/WindowContext.hpp>

namespace Assisi::Render
{
class RenderSystem
{
  public:
    RenderSystem() = delete;

    static bool Initialize(Backend::GraphicsBackend graphicsBackend, const Assisi::Window::WindowContext &window)
    {
        if (!window.IsValid())
        {
            std::cout << "RenderSystem: Window is not valid." << std::endl;
            return false;
        }

        if (graphicsBackend == Backend::GraphicsBackend::None)
        {
            std::cout << "RenderSystem: No graphics backend selected." << std::endl;
            return false;
        }

        if (graphicsBackend == Backend::GraphicsBackend::OpenGL)
        {
            return InitializeOpenGL(window);
        }

        if (graphicsBackend == Backend::GraphicsBackend::Vulkan)
        {
            return InitializeVulkan(window);
        }

        std::cout << "RenderSystem: Unsupported graphics backend." << std::endl;
        return false;
    }

  private:
    static bool InitializeOpenGL(const Assisi::Window::WindowContext &window);

    static bool InitializeVulkan(const Assisi::Window::WindowContext &window)
    {
        /* Vulkan initialization will live in Assisi::Render::Vulkan later. */
        std::cout << "RenderSystem: Vulkan backend is not implemented yet." << std::endl;
        (void)window;
        return false;
    }
};
} /* namespace Assisi::Render */

#endif /* RENDERSYSTEM_HPP */