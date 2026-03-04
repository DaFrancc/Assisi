#pragma once

/// @file RenderSystem.hpp
/// @brief Entry point for initializing the graphics backend.

#include <iostream>

#include <Assisi/Render/Backend/GraphicsBackend.hpp>
#include <Assisi/Window/WindowContext.hpp>

namespace Assisi::Render
{
/// @brief Static service that initializes and owns the graphics backend.
///
/// Call Initialize() once after creating a WindowContext.  Only OpenGL is
/// currently implemented; Vulkan returns false immediately.
class RenderSystem
{
  public:
    RenderSystem() = delete;

    /// @brief Initializes the chosen graphics backend against the given window.
    ///
    /// Validates that the window is live and that a supported backend was
    /// requested, then delegates to the appropriate backend initializer.
    ///
    /// @param graphicsBackend  The backend to initialize.
    /// @param window           A valid, current WindowContext.
    /// @return true on success, false on any error (logged to stdout).
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
    /// @brief Loads OpenGL function pointers via Glad and configures initial state.
    static bool InitializeOpenGL(const Assisi::Window::WindowContext &window);

    /// @brief Stub — Vulkan support is not yet implemented.
    static bool InitializeVulkan(const Assisi::Window::WindowContext &window)
    {
        /* Vulkan initialization will live in Assisi::Render::Vulkan later. */
        std::cout << "RenderSystem: Vulkan backend is not implemented yet." << std::endl;
        (void)window;
        return false;
    }
};
} /* namespace Assisi::Render */