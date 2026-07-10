/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file VulkanContext.hpp
/// @brief Owns the Vulkan instance/device/swapchain and wraps them in an NVRHI device.
///
/// NVRHI does not create the window surface, physical device, logical device, or
/// swapchain — those are plain Vulkan API calls the application is responsible for.
/// This class owns that bring-up (proven first in the standalone apps/vk_triangle
/// spike) and hands NVRHI an already-created VkInstance/VkPhysicalDevice/VkDevice.

#include <vulkan/vulkan.h>

#include <nvrhi/nvrhi.h>
#include <nvrhi/vulkan.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <Assisi/Render/RenderFrame.hpp>

namespace Assisi::Window
{
class WindowContext;
}

namespace Assisi::Render::Vulkan
{

/// @brief Owns the Vulkan instance/device/swapchain and the NVRHI device wrapping them.
///
/// Move-only; there is only ever meant to be one of these per window. Construct via
/// Create(), which returns nullptr on any failure (logging the reason).
class VulkanContext
{
  public:
    /// @brief Creates the Vulkan instance, device, and swapchain for the given window.
    /// @return nullptr on failure.
    static std::unique_ptr<VulkanContext> Create(const Assisi::Window::WindowContext &window);

    ~VulkanContext();

    VulkanContext(const VulkanContext &) = delete;
    VulkanContext &operator=(const VulkanContext &) = delete;
    VulkanContext(VulkanContext &&) = delete;
    VulkanContext &operator=(VulkanContext &&) = delete;

    /// @brief Acquires the next swapchain image and opens a command list for the frame.
    /// @return std::nullopt if acquisition failed (e.g. window minimized to zero size).
    std::optional<RenderFrame> BeginFrame();

    /// @brief Closes and submits the command list opened by BeginFrame(), then presents.
    void EndFrame();

    /// @brief Recreates the swapchain (and its dependent NVRHI textures/framebuffers)
    /// at the new framebuffer size. Waits for the device to go idle first.
    void Resize(uint32_t width, uint32_t height);

    [[nodiscard]] nvrhi::IDevice *GetDevice() const { return _nvrhiDevice; }

    /// @brief Color/depth formats and sample count of the swapchain framebuffers,
    /// for constructing pipelines compatible with them ahead of the first frame.
    [[nodiscard]] nvrhi::FramebufferInfo GetFramebufferInfo() const { return _framebuffers.at(0)->getFramebufferInfo(); }

    /// @name Raw Vulkan handles
    /// For integrating libraries that talk raw Vulkan directly instead of going
    /// through NVRHI — currently just Dear ImGui's Vulkan backend (see DebugUI.cpp).
    /// Prefer nvrhi::IDevice / getNativeObject() for anything else.
    ///@{
    [[nodiscard]] VkInstance GetVkInstance() const { return _instance; }
    [[nodiscard]] VkPhysicalDevice GetVkPhysicalDevice() const { return _physicalDevice; }
    [[nodiscard]] VkDevice GetVkDevice() const { return _device; }
    [[nodiscard]] VkQueue GetVkGraphicsQueue() const { return _graphicsQueue; }
    [[nodiscard]] uint32_t GetVkGraphicsQueueFamily() const { return _graphicsQueueFamily; }
    [[nodiscard]] uint32_t GetSwapchainImageCount() const { return static_cast<uint32_t>(_swapchainImages.size()); }
    [[nodiscard]] VkFormat GetSwapchainFormat() const { return _swapchainFormat; }

    /// Depth-stencil format chosen for the swapchain depth buffer. Queried
    /// against the device at swapchain creation (D24S8 -> D32S8 -> D32), so
    /// consumers must read it rather than assume D24S8. May be a depth-only
    /// format (D32_SFLOAT) with no stencil aspect.
    [[nodiscard]] VkFormat GetDepthFormat() const { return _depthFormat; }
    ///@}

  private:
    VulkanContext() = default;

    void DestroySwapchainResources();
    [[nodiscard]] bool CreateSwapchainResources(uint32_t width, uint32_t height);

    /// Installs the validation debug messenger in debug builds; no-op in
    /// release and when the debug-utils extension wasn't enabled.
    void CreateDebugMessenger();

    VkInstance       _instance = VK_NULL_HANDLE;
    VkSurfaceKHR     _surface = VK_NULL_HANDLE;
    VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
    VkDevice         _device = VK_NULL_HANDLE;
    VkQueue          _graphicsQueue = VK_NULL_HANDLE;
    uint32_t         _graphicsQueueFamily = 0;

    VkSwapchainKHR        _swapchain = VK_NULL_HANDLE;
    VkFormat              _swapchainFormat = VK_FORMAT_UNDEFINED;
    VkFormat              _depthFormat = VK_FORMAT_UNDEFINED;

    VkDebugUtilsMessengerEXT _debugMessenger = VK_NULL_HANDLE;
    VkExtent2D            _swapchainExtent{};
    std::vector<VkImage>  _swapchainImages;

    nvrhi::vulkan::DeviceHandle            _nvrhiDeviceHandle;
    nvrhi::vulkan::IDevice                *_nvrhiDevice = nullptr;
    std::vector<nvrhi::TextureHandle>      _swapchainTextures;
    nvrhi::TextureHandle                   _depthTexture;
    std::vector<nvrhi::FramebufferHandle>  _framebuffers;

    // A single semaphore pair: BeginFrame() calls waitForIdle(), so only one
    // frame is ever in flight. Multi-frame pipelining would need per-frame
    // fences and a render-finished semaphore per swapchain image — deferred
    // until there's a perf reason (see the note in BeginFrame()).
    VkSemaphore              _imageAvailableSemaphore = VK_NULL_HANDLE;
    VkSemaphore              _renderFinishedSemaphore = VK_NULL_HANDLE;
    nvrhi::CommandListHandle _commandList;

    uint32_t _currentImageIndex = 0;
};

} // namespace Assisi::Render::Vulkan
