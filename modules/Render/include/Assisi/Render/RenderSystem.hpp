#pragma once

/// @file RenderSystem.hpp
/// @brief Entry point for initializing the graphics backend.

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/Backend/GraphicsBackend.hpp>
#include <Assisi/Render/Vulkan/VulkanContext.hpp>
#include <Assisi/Window/WindowContext.hpp>

#include <memory>

namespace Assisi::Render
{
/// @brief Static service that initializes and owns the graphics backend.
///
/// Call Initialize() once after creating a WindowContext.
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
    /// @return true on success, false on any error.
    static bool Initialize(Backend::GraphicsBackend graphicsBackend, const Assisi::Window::WindowContext &window)
    {
        if (!window.IsValid())
        {
            Assisi::Core::Log::Error("RenderSystem: Window is not valid.");
            return false;
        }

        if (graphicsBackend == Backend::GraphicsBackend::None)
        {
            Assisi::Core::Log::Error("RenderSystem: No graphics backend selected.");
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

        Assisi::Core::Log::Error("RenderSystem: Unsupported graphics backend.");
        return false;
    }

    /// @brief Returns the Vulkan context created by Initialize(Vulkan, ...), or
    /// nullptr if the Vulkan backend was never (successfully) initialized.
    static Vulkan::VulkanContext *GetVulkanContext() { return s_vulkanContext.get(); }

  private:
    /// @brief Loads OpenGL function pointers via Glad and configures initial state.
    static bool InitializeOpenGL(const Assisi::Window::WindowContext &window);

    static bool InitializeVulkan(const Assisi::Window::WindowContext &window)
    {
        s_vulkanContext = Vulkan::VulkanContext::Create(window);
        return s_vulkanContext != nullptr;
    }

    static inline std::unique_ptr<Vulkan::VulkanContext> s_vulkanContext;
};
} /* namespace Assisi::Render */