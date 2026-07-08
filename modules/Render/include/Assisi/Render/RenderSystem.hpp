/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file RenderSystem.hpp
/// @brief Entry point for initializing the Vulkan/NVRHI graphics backend.

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/Vulkan/VulkanContext.hpp>
#include <Assisi/Window/WindowContext.hpp>

#include <memory>

namespace Assisi::Render
{
/// @brief Static service that initializes and owns the Vulkan/NVRHI device.
///
/// Call Initialize() once after creating a WindowContext.
///
/// @note Intentional service-locator: there is exactly one GPU device per
/// process, created once and torn down at exit, and it underpins nearly every
/// render call site. Threading it through as a member everywhere buys little
/// over an owned-by-service singleton. If it ever needs to be swappable (e.g.
/// for a headless test device), promote GetVulkanContext() to an injected
/// reference then; today it is deliberately a global.
class RenderSystem
{
  public:
    RenderSystem() = delete;

    /// @brief Initializes the Vulkan/NVRHI backend against the given window.
    ///
    /// @param window  A valid, current WindowContext.
    /// @return true on success, false on any error.
    [[nodiscard]] static bool Initialize(const Assisi::Window::WindowContext &window)
    {
        if (!window.IsValid())
        {
            Assisi::Core::Log::Error("RenderSystem: Window is not valid.");
            return false;
        }

        s_vulkanContext = Vulkan::VulkanContext::Create(window);
        return s_vulkanContext != nullptr;
    }

    /// @brief Returns the Vulkan context created by Initialize(), or nullptr if
    /// initialization hasn't happened (or failed).
    static Vulkan::VulkanContext *GetVulkanContext() { return s_vulkanContext.get(); }

  private:
    static inline std::unique_ptr<Vulkan::VulkanContext> s_vulkanContext;
};
} /* namespace Assisi::Render */
