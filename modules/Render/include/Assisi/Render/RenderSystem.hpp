#pragma once

/// @file RenderSystem.hpp
/// @brief Entry point for initializing the Vulkan/NVRHI graphics backend.

#include <Assisi/Render/Vulkan/VulkanContext.hpp>
#include <Assisi/Window/WindowContext.hpp>

#include <memory>

namespace Assisi::Render
{
/// @brief Static service that initializes and owns the Vulkan/NVRHI device.
///
/// Call Initialize() once after creating a WindowContext, and Shutdown() once
/// before process exit.
class RenderSystem
{
  public:
    RenderSystem() = delete;

    /// @brief Initializes the Vulkan/NVRHI backend against the given window.
    ///
    /// Also starts glslang's global process state — required before any
    /// `Render::CompileGlslShader` call (see ShaderModule.hpp).
    ///
    /// @param window  A valid, current WindowContext.
    /// @return true on success, false on any error.
    static bool Initialize(const Assisi::Window::WindowContext &window);

    /// @brief Tears down the Vulkan/NVRHI backend and glslang's global process state.
    static void Shutdown();

    /// @brief Returns the Vulkan context created by Initialize(), or nullptr if
    /// initialization hasn't happened (or failed).
    static Vulkan::VulkanContext *GetVulkanContext() { return s_vulkanContext.get(); }

  private:
    static inline std::unique_ptr<Vulkan::VulkanContext> s_vulkanContext;
};
} /* namespace Assisi::Render */
