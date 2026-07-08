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

namespace Assisi::Window
{
class WindowContext;
}

namespace Assisi::Render::Vulkan
{

/// @brief Resources for the swapchain image acquired for the current frame.
struct VulkanFrame
{
    nvrhi::ICommandList *commandList = nullptr;
    nvrhi::ITexture     *colorTexture = nullptr;
    nvrhi::ITexture     *depthTexture = nullptr;
    nvrhi::IFramebuffer *framebuffer = nullptr;
    uint32_t             width = 0;
    uint32_t             height = 0;
};

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
    std::optional<VulkanFrame> BeginFrame();

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
    ///@}

  private:
    VulkanContext() = default;

    void DestroySwapchainResources();
    bool CreateSwapchainResources(uint32_t width, uint32_t height);

    static constexpr uint32_t kFramesInFlight = 2;

    VkInstance       _instance = VK_NULL_HANDLE;
    VkSurfaceKHR     _surface = VK_NULL_HANDLE;
    VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
    VkDevice         _device = VK_NULL_HANDLE;
    VkQueue          _graphicsQueue = VK_NULL_HANDLE;
    uint32_t         _graphicsQueueFamily = 0;

    VkSwapchainKHR        _swapchain = VK_NULL_HANDLE;
    VkFormat              _swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D            _swapchainExtent{};
    std::vector<VkImage>  _swapchainImages;

    nvrhi::vulkan::DeviceHandle            _nvrhiDeviceHandle;
    nvrhi::vulkan::IDevice                *_nvrhiDevice = nullptr;
    std::vector<nvrhi::TextureHandle>      _swapchainTextures;
    nvrhi::TextureHandle                   _depthTexture;
    std::vector<nvrhi::FramebufferHandle>  _framebuffers;

    std::vector<VkSemaphore> _imageAvailableSemaphores;
    std::vector<VkSemaphore> _renderFinishedSemaphores;
    nvrhi::CommandListHandle _commandList;

    uint32_t _frameIndex = 0;
    uint32_t _currentImageIndex = 0;
};

} // namespace Assisi::Render::Vulkan
