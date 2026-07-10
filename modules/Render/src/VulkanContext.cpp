/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file VulkanContext.cpp
///
/// There is no Vulkan SDK assumed to be installed (driver runtime only), so this
/// uses vulkan.hpp's dynamic dispatcher instead of linking a static loader import
/// lib — mirroring NVIDIA's own Donut framework (DeviceManager_VK.cpp).

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include <GLFW/glfw3.h>

#include <array>
#include <vector>
#ifndef NDEBUG
#include <cstring> // std::strcmp, validation-layer lookup (debug only)
#endif

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/Vulkan/VulkanContext.hpp>
#include <Assisi/Window/WindowContext.hpp>

#define VKD VULKAN_HPP_DEFAULT_DISPATCHER

namespace Assisi::Render::Vulkan
{
namespace
{

#ifndef NDEBUG
constexpr const char *kValidationLayer = "VK_LAYER_KHRONOS_validation";

// Routes every validation message into Core::Log. Returns VK_FALSE so the
// offending Vulkan call is NOT aborted — we want to observe, not intercept.
VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT       severity,
    VkDebugUtilsMessageTypeFlagsEXT              /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT  *data,
    void                                        * /*userData*/)
{
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        Core::Log::Error("[Vulkan] {}", data->pMessage);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        Core::Log::Warn("[Vulkan] {}", data->pMessage);
    else
        Core::Log::Info("[Vulkan] {}", data->pMessage);
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT MakeDebugMessengerCreateInfo()
{
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    // Warnings + errors only; the info/verbose streams are noise at this stage.
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = DebugCallback;
    return info;
}

bool IsValidationLayerAvailable()
{
    uint32_t count = 0;
    VKD.vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    VKD.vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties &layer : layers)
        if (std::strcmp(layer.layerName, kValidationLayer) == 0)
            return true;
    return false;
}
#endif // !NDEBUG

VkInstance CreateInstance()
{
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Assisi";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "Assisi";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    uint32_t glfwExtensionCount = 0;
    const char **glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    std::vector<const char *> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
    std::vector<const char *> layers;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

#ifndef NDEBUG
    // Enable the Khronos validation layer + debug-utils in debug builds so
    // spec violations become log messages instead of silent UB. Chaining the
    // messenger create-info onto pNext also captures issues raised during
    // vkCreateInstance / vkDestroyInstance themselves.
    VkDebugUtilsMessengerCreateInfoEXT debugInfo = MakeDebugMessengerCreateInfo();
    if (IsValidationLayerAvailable())
    {
        layers.push_back(kValidationLayer);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        createInfo.pNext = &debugInfo;
    }
    else
    {
        Core::Log::Warn("VulkanContext: validation layer '{}' not available; "
                        "install the Vulkan SDK to enable GPU debug output.",
                        kValidationLayer);
    }
#endif

    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    createInfo.ppEnabledLayerNames = layers.data();

    VkInstance instance = VK_NULL_HANDLE;
    if (VKD.vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }
    return instance;
}

struct PhysicalDeviceChoice
{
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily = 0;
};

std::optional<PhysicalDeviceChoice> ChoosePhysicalDevice(VkInstance instance, VkSurfaceKHR surface)
{
    uint32_t deviceCount = 0;
    VKD.vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0)
    {
        return std::nullopt;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    VKD.vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    std::optional<PhysicalDeviceChoice> best;

    for (VkPhysicalDevice device : devices)
    {
        uint32_t queueFamilyCount = 0;
        VKD.vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        VKD.vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        std::optional<uint32_t> graphicsFamily;
        for (uint32_t i = 0; i < queueFamilyCount; ++i)
        {
            const bool hasGraphics = (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;

            VkBool32 presentSupport = VK_FALSE;
            VKD.vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);

            if (hasGraphics && presentSupport == VK_TRUE)
            {
                graphicsFamily = i;
                break;
            }
        }

        if (!graphicsFamily.has_value())
        {
            continue;
        }

        VkPhysicalDeviceProperties props{};
        VKD.vkGetPhysicalDeviceProperties(device, &props);

        const bool isDiscrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

        Core::Log::Info("Vulkan candidate: {} ({})", props.deviceName,
                         isDiscrete ? "discrete" : "integrated/other");

        if (!best.has_value() || isDiscrete)
        {
            best = PhysicalDeviceChoice{device, *graphicsFamily};
            if (isDiscrete)
            {
                break;
            }
        }
    }

    return best;
}

VkDevice CreateLogicalDevice(VkPhysicalDevice physicalDevice, uint32_t graphicsQueueFamily)
{
    const float queuePriority = 1.0f;

    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = graphicsQueueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    const char *deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    // NVRHI's Vulkan backend uses these features unconditionally and assumes the
    // caller enabled them at device creation: timeline semaphores (queue
    // tracking), synchronization2 (vkCmdPipelineBarrier2), and dynamic rendering
    // (it emits no render-pass objects). All are Vulkan 1.3 core — enable them
    // through the promoted feature structs chained onto pNext. Without this the
    // driver silently tolerates the violations on some vendors (NVIDIA) while
    // the barrier2 entry point isn't even loaded, which asserts in debug.
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.synchronization2 = VK_TRUE;
    features13.dynamicRendering = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.timelineSemaphore = VK_TRUE;
    features12.pNext = &features13;

    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.pNext = &features12;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount = 1;
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions;

    VkDevice device = VK_NULL_HANDLE;
    if (VKD.vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device) != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }
    return device;
}

nvrhi::Format ToNvrhiFormat(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_B8G8R8A8_UNORM: return nvrhi::Format::BGRA8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:  return nvrhi::Format::SBGRA8_UNORM;
    default:                       return nvrhi::Format::UNKNOWN;
    }
}

struct DepthFormatChoice
{
    VkFormat      vk        = VK_FORMAT_UNDEFINED;
    nvrhi::Format nvrhiFmt  = nvrhi::Format::UNKNOWN;
};

// The Vulkan spec only guarantees that ONE of D24_UNORM_S8_UINT /
// D32_SFLOAT_S8_UINT is usable as a depth-stencil attachment; D24S8 in
// particular is absent on much AMD hardware. Query the device and fall back
// D24S8 -> D32S8 -> D32 (depth-only). Returns {UNDEFINED, UNKNOWN} if the
// device somehow supports none, which the caller treats as a hard failure.
DepthFormatChoice ChooseDepthFormat(VkPhysicalDevice physicalDevice)
{
    const std::array<DepthFormatChoice, 3> candidates{{
        { VK_FORMAT_D24_UNORM_S8_UINT,  nvrhi::Format::D24S8 },
        { VK_FORMAT_D32_SFLOAT_S8_UINT, nvrhi::Format::D32S8 },
        { VK_FORMAT_D32_SFLOAT,         nvrhi::Format::D32   },
    }};

    for (const DepthFormatChoice &candidate : candidates)
    {
        VkFormatProperties props{};
        VKD.vkGetPhysicalDeviceFormatProperties(physicalDevice, candidate.vk, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return candidate;
    }

    return {};
}

} // namespace

std::unique_ptr<VulkanContext> VulkanContext::Create(const Assisi::Window::WindowContext &window)
{
    // Bootstrap the dynamic dispatcher: load vulkan-1.dll ourselves and resolve
    // vkGetInstanceProcAddr from it, since there's no SDK import lib to link.
    static vk::detail::DynamicLoader s_dynamicLoader;
    static bool s_dispatcherBootstrapped = false;
    if (!s_dispatcherBootstrapped)
    {
        auto getInstanceProcAddr = s_dynamicLoader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
        VKD.init(getInstanceProcAddr);
        s_dispatcherBootstrapped = true;
    }

    auto context = std::unique_ptr<VulkanContext>(new VulkanContext());

    context->_instance = CreateInstance();
    if (context->_instance == VK_NULL_HANDLE)
    {
        Core::Log::Error("VulkanContext: vkCreateInstance failed.");
        return nullptr;
    }
    VKD.init(vk::Instance(context->_instance));
    context->CreateDebugMessenger();

    if (glfwCreateWindowSurface(context->_instance, window.NativeHandle(), nullptr, &context->_surface) != VK_SUCCESS)
    {
        Core::Log::Error("VulkanContext: glfwCreateWindowSurface failed.");
        return nullptr;
    }

    auto physicalDeviceChoice = ChoosePhysicalDevice(context->_instance, context->_surface);
    if (!physicalDeviceChoice.has_value())
    {
        Core::Log::Error("VulkanContext: no suitable Vulkan physical device found.");
        return nullptr;
    }
    context->_physicalDevice = physicalDeviceChoice->physicalDevice;
    context->_graphicsQueueFamily = physicalDeviceChoice->graphicsQueueFamily;

    VkPhysicalDeviceProperties chosenProps{};
    VKD.vkGetPhysicalDeviceProperties(context->_physicalDevice, &chosenProps);
    Core::Log::Info("VulkanContext: selected device {}", chosenProps.deviceName);

    context->_device = CreateLogicalDevice(context->_physicalDevice, context->_graphicsQueueFamily);
    if (context->_device == VK_NULL_HANDLE)
    {
        Core::Log::Error("VulkanContext: vkCreateDevice failed.");
        return nullptr;
    }
    VKD.init(vk::Device(context->_device));

    VKD.vkGetDeviceQueue(context->_device, context->_graphicsQueueFamily, 0, &context->_graphicsQueue);

    const char *deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    nvrhi::vulkan::DeviceDesc nvrhiDeviceDesc;
    nvrhiDeviceDesc.instance = context->_instance;
    nvrhiDeviceDesc.physicalDevice = context->_physicalDevice;
    nvrhiDeviceDesc.device = context->_device;
    nvrhiDeviceDesc.graphicsQueue = context->_graphicsQueue;
    nvrhiDeviceDesc.graphicsQueueIndex = static_cast<int>(context->_graphicsQueueFamily);
    nvrhiDeviceDesc.deviceExtensions = deviceExtensions;
    nvrhiDeviceDesc.numDeviceExtensions = 1;

    context->_nvrhiDeviceHandle = nvrhi::vulkan::createDevice(nvrhiDeviceDesc);
    if (!context->_nvrhiDeviceHandle)
    {
        Core::Log::Error("VulkanContext: nvrhi::vulkan::createDevice failed.");
        return nullptr;
    }
    context->_nvrhiDevice = context->_nvrhiDeviceHandle;

    const Window::WindowSize fbSize = window.GetFramebufferSize();
    if (!context->CreateSwapchainResources(static_cast<uint32_t>(fbSize.Width), static_cast<uint32_t>(fbSize.Height)))
    {
        Core::Log::Error("VulkanContext: initial swapchain creation failed.");
        return nullptr;
    }

    {
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (VKD.vkCreateSemaphore(context->_device, &semInfo, nullptr, &context->_imageAvailableSemaphore) != VK_SUCCESS ||
            VKD.vkCreateSemaphore(context->_device, &semInfo, nullptr, &context->_renderFinishedSemaphore) != VK_SUCCESS)
        {
            Core::Log::Error("VulkanContext: vkCreateSemaphore failed.");
            return nullptr;
        }
    }

    context->_commandList = context->_nvrhiDevice->createCommandList();

    return context;
}

bool VulkanContext::CreateSwapchainResources(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return false; // minimized — nothing to create yet
    }

    VkSurfaceCapabilitiesKHR capabilities{};
    if (VKD.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_physicalDevice, _surface, &capabilities) != VK_SUCCESS)
    {
        Core::Log::Error("VulkanContext: vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed.");
        return false;
    }

    uint32_t formatCount = 0;
    VKD.vkGetPhysicalDeviceSurfaceFormatsKHR(_physicalDevice, _surface, &formatCount, nullptr);
    if (formatCount == 0)
    {
        Core::Log::Error("VulkanContext: surface reports no available formats.");
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    VKD.vkGetPhysicalDeviceSurfaceFormatsKHR(_physicalDevice, _surface, &formatCount, formats.data());

    VkSurfaceFormatKHR chosenFormat = formats[0];
    for (const auto &f : formats)
    {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            chosenFormat = f;
            break;
        }
    }

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == 0xFFFFFFFF)
    {
        extent.width = width;
        extent.height = height;
    }

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount)
    {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainKHR oldSwapchain = _swapchain;

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = _surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = chosenFormat.format;
    createInfo.imageColorSpace = chosenFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = ChoosePresentMode();
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = oldSwapchain;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    if (VKD.vkCreateSwapchainKHR(_device, &createInfo, nullptr, &newSwapchain) != VK_SUCCESS)
    {
        Core::Log::Error("VulkanContext: vkCreateSwapchainKHR failed.");
        return false;
    }

    // Tear down the resources tied to the old swapchain now that the new one exists.
    DestroySwapchainResources();
    if (oldSwapchain != VK_NULL_HANDLE)
    {
        VKD.vkDestroySwapchainKHR(_device, oldSwapchain, nullptr);
    }

    _swapchain = newSwapchain;
    _swapchainFormat = chosenFormat.format;
    _swapchainExtent = extent;

    uint32_t actualImageCount = 0;
    VKD.vkGetSwapchainImagesKHR(_device, _swapchain, &actualImageCount, nullptr);
    _swapchainImages.resize(actualImageCount);
    VKD.vkGetSwapchainImagesKHR(_device, _swapchain, &actualImageCount, _swapchainImages.data());

    const nvrhi::Format colorFormat = ToNvrhiFormat(_swapchainFormat);
    if (colorFormat == nvrhi::Format::UNKNOWN)
    {
        Core::Log::Error("VulkanContext: swapchain format {} has no NVRHI mapping.",
                         static_cast<int>(_swapchainFormat));
        return false;
    }

    for (VkImage image : _swapchainImages)
    {
        nvrhi::TextureDesc textureDesc;
        textureDesc.width = _swapchainExtent.width;
        textureDesc.height = _swapchainExtent.height;
        textureDesc.format = colorFormat;
        textureDesc.isRenderTarget = true;
        textureDesc.debugName = "SwapchainImage";
        textureDesc.enableAutomaticStateTracking(nvrhi::ResourceStates::Present);

        nvrhi::TextureHandle texture = _nvrhiDevice->createHandleForNativeTexture(
            nvrhi::ObjectTypes::VK_Image, nvrhi::Object(image), textureDesc);
        _swapchainTextures.push_back(texture);
    }

    const DepthFormatChoice depthFormat = ChooseDepthFormat(_physicalDevice);
    if (depthFormat.nvrhiFmt == nvrhi::Format::UNKNOWN)
    {
        Core::Log::Error("VulkanContext: no supported depth-stencil format found.");
        return false;
    }
    _depthFormat = depthFormat.vk;

    nvrhi::TextureDesc depthDesc;
    depthDesc.width = _swapchainExtent.width;
    depthDesc.height = _swapchainExtent.height;
    depthDesc.format = depthFormat.nvrhiFmt;
    depthDesc.isRenderTarget = true;
    depthDesc.debugName = "DepthBuffer";
    depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
    depthDesc.keepInitialState = true;
    _depthTexture = _nvrhiDevice->createTexture(depthDesc);

    for (const nvrhi::TextureHandle &colorTexture : _swapchainTextures)
    {
        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(colorTexture);
        fbDesc.setDepthAttachment(_depthTexture);
        _framebuffers.push_back(_nvrhiDevice->createFramebuffer(fbDesc));
    }

    return true;
}

void VulkanContext::DestroySwapchainResources()
{
    _framebuffers.clear();
    _depthTexture = nullptr;
    _swapchainTextures.clear();
    _swapchainImages.clear();
}

void VulkanContext::Resize(uint32_t width, uint32_t height)
{
    if (_device == VK_NULL_HANDLE)
    {
        return;
    }

    VKD.vkDeviceWaitIdle(_device);

    // A failure here is benign and intentionally not propagated: a zero-size
    // (minimized) window returns false, and if recreation fails the previous
    // swapchain is left intact, so BeginFrame() keeps working / recovers on a
    // later resize. Explicitly discarded to satisfy [[nodiscard]].
    (void)CreateSwapchainResources(width, height);
}

VkPresentModeKHR VulkanContext::ChoosePresentMode() const
{
    // FIFO (vsync) is the only present mode the spec guarantees is available, so
    // it needs no capability query.
    if (_vsyncEnabled)
    {
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    uint32_t count = 0;
    VKD.vkGetPhysicalDeviceSurfacePresentModesKHR(_physicalDevice, _surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    VKD.vkGetPhysicalDeviceSurfacePresentModesKHR(_physicalDevice, _surface, &count, modes.data());

    for (VkPresentModeKHR mode : modes)
    {
        if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
        {
            return mode;
        }
    }

    Core::Log::Warn("VulkanContext: IMMEDIATE present mode unsupported; vsync-off falls back to FIFO.");
    return VK_PRESENT_MODE_FIFO_KHR;
}

void VulkanContext::SetVSync(bool enabled)
{
    if (_vsyncEnabled == enabled || _device == VK_NULL_HANDLE)
    {
        return;
    }

    _vsyncEnabled = enabled;
    VKD.vkDeviceWaitIdle(_device);

    // Recreate at the current extent with the new present mode. Benign on failure,
    // same as Resize() — the old swapchain is left intact.
    (void)CreateSwapchainResources(_swapchainExtent.width, _swapchainExtent.height);
}

std::optional<RenderFrame> VulkanContext::BeginFrame()
{
    if (_swapchain == VK_NULL_HANDLE)
    {
        return std::nullopt; // minimized; Resize() hasn't (re)created anything yet
    }

    // Wait for the previous frame's GPU work to fully finish before reusing
    // anything. Without this, resources NVRHI doesn't track the lifetime of —
    // notably Dear ImGui's own raw Vulkan vertex/index buffers, which it
    // round-robins across frames assuming the caller throttles submission —
    // can get overwritten by the CPU while the GPU is still reading them from
    // an earlier frame, producing visible corruption under any per-frame-varying
    // content (most obvious while dragging/resizing ImGui windows). A real
    // multi-frame-in-flight fence would let CPU and GPU overlap more; this
    // trades that overlap for simplicity/correctness while the migration is
    // still settling — worth revisiting once there's a perf reason to.
    _nvrhiDevice->waitForIdle();

    VkResult acquireResult = VKD.vkAcquireNextImageKHR(_device, _swapchain, UINT64_MAX, _imageAvailableSemaphore,
                                                        VK_NULL_HANDLE, &_currentImageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        // Swapchain is stale; the next explicit Resize() (from the window resize
        // callback) will recreate it. Skip rendering this frame.
        return std::nullopt;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
    {
        Core::Log::Error("VulkanContext: vkAcquireNextImageKHR failed with VkResult {}",
                          static_cast<int>(acquireResult));
        return std::nullopt;
    }

    _commandList->open();
    _commandList->setTextureState(_swapchainTextures[_currentImageIndex], nvrhi::AllSubresources,
                                   nvrhi::ResourceStates::RenderTarget);

    RenderFrame frame;
    frame.commandList = _commandList;
    frame.colorTexture = _swapchainTextures[_currentImageIndex];
    frame.depthTexture = _depthTexture;
    frame.framebuffer = _framebuffers[_currentImageIndex];
    frame.width = _swapchainExtent.width;
    frame.height = _swapchainExtent.height;
    return frame;
}

void VulkanContext::EndFrame()
{
    _commandList->close();

    _nvrhiDevice->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, _imageAvailableSemaphore, 0);
    _nvrhiDevice->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, _renderFinishedSemaphore, 0);
    _nvrhiDevice->executeCommandList(_commandList);

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &_renderFinishedSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &_swapchain;
    presentInfo.pImageIndices = &_currentImageIndex;
    const VkResult presentResult = VKD.vkQueuePresentKHR(_graphicsQueue, &presentInfo);
    // OUT_OF_DATE/SUBOPTIMAL are expected on resize; the window resize callback
    // recreates the swapchain, so they're not errors here. Anything else is.
    if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR &&
        presentResult != VK_ERROR_OUT_OF_DATE_KHR)
    {
        Core::Log::Error("VulkanContext: vkQueuePresentKHR failed with VkResult {}",
                         static_cast<int>(presentResult));
    }

    _nvrhiDevice->runGarbageCollection();
}

void VulkanContext::CreateDebugMessenger()
{
#ifndef NDEBUG
    // Null when the debug-utils extension wasn't enabled (validation layer
    // absent) — the dispatcher only loads the entry point if the instance
    // advertised the extension.
    if (!VKD.vkCreateDebugUtilsMessengerEXT)
        return;

    VkDebugUtilsMessengerCreateInfoEXT info = MakeDebugMessengerCreateInfo();
    if (VKD.vkCreateDebugUtilsMessengerEXT(_instance, &info, nullptr, &_debugMessenger) != VK_SUCCESS)
        Core::Log::Warn("VulkanContext: vkCreateDebugUtilsMessengerEXT failed.");
#endif
}

VulkanContext::~VulkanContext()
{
    // Guard each teardown on its own handle rather than gating everything on
    // _device: Create() can fail after the instance and surface exist but
    // before the device does, and those still need to be destroyed. VKD's
    // instance-level functions are loaded as soon as _instance is created.
    if (_device != VK_NULL_HANDLE)
    {
        VKD.vkDeviceWaitIdle(_device);

        _commandList = nullptr;
        DestroySwapchainResources();
        _nvrhiDevice = nullptr;
        _nvrhiDeviceHandle = nullptr;

        if (_imageAvailableSemaphore != VK_NULL_HANDLE) VKD.vkDestroySemaphore(_device, _imageAvailableSemaphore, nullptr);
        if (_renderFinishedSemaphore != VK_NULL_HANDLE) VKD.vkDestroySemaphore(_device, _renderFinishedSemaphore, nullptr);

        if (_swapchain != VK_NULL_HANDLE) VKD.vkDestroySwapchainKHR(_device, _swapchain, nullptr);
        VKD.vkDestroyDevice(_device, nullptr);
    }

    if (_surface != VK_NULL_HANDLE) VKD.vkDestroySurfaceKHR(_instance, _surface, nullptr);

    if (_debugMessenger != VK_NULL_HANDLE)
        VKD.vkDestroyDebugUtilsMessengerEXT(_instance, _debugMessenger, nullptr);

    if (_instance != VK_NULL_HANDLE) VKD.vkDestroyInstance(_instance, nullptr);
}

} // namespace Assisi::Render::Vulkan
