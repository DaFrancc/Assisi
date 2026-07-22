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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring> // std::strcmp — device-extension and validation-layer name comparisons
#include <vector>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Platform.hpp>
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

// The requirements CreateLogicalDevice enables unconditionally: a Vulkan 1.3
// device (the feature structs it chains are 1.3-promoted), the swapchain
// extension, and the timeline-semaphore / synchronization2 / dynamic-rendering
// features NVRHI assumes. Verified here so selection and creation agree on
// requirements — a device that can't satisfy them is skipped in favour of one
// that can, instead of being chosen and then failing vkCreateDevice with a
// generic error (e.g. a compute-only or pre-1.3 adapter enumerated first).
bool DeviceMeetsRequirements(VkPhysicalDevice device, const VkPhysicalDeviceProperties &props)
{
    if (props.apiVersion < VK_API_VERSION_1_3)
    {
        Core::Log::Info("  rejected: reports Vulkan {}.{}, need 1.3",
                        VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
        return false;
    }

    uint32_t extensionCount = 0;
    VKD.vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    VKD.vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());

    const bool hasSwapchain = std::any_of(extensions.begin(), extensions.end(),
                                          [](const VkExtensionProperties &ext) {
                                              return std::strcmp(ext.extensionName,
                                                                 VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
                                          });
    if (!hasSwapchain)
    {
        Core::Log::Info("  rejected: missing {}", VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        return false;
    }

    // Query the promoted feature structs (core since Vulkan 1.1, and the instance
    // requests 1.3) to confirm the three NVRHI relies on are actually supported.
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = &features13;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features12;

    VKD.vkGetPhysicalDeviceFeatures2(device, &features2);

    if (features12.timelineSemaphore != VK_TRUE || features13.synchronization2 != VK_TRUE ||
        features13.dynamicRendering != VK_TRUE || features13.shaderDemoteToHelperInvocation != VK_TRUE)
    {
        Core::Log::Info("  rejected: missing a required feature (timelineSemaphore={}, "
                        "synchronization2={}, dynamicRendering={}, shaderDemoteToHelperInvocation={})",
                        features12.timelineSemaphore == VK_TRUE, features13.synchronization2 == VK_TRUE,
                        features13.dynamicRendering == VK_TRUE,
                        features13.shaderDemoteToHelperInvocation == VK_TRUE);
        return false;
    }

    // Descriptor indexing / bindless (GPU-driven stage D). These are Vulkan 1.2
    // *core* but *optional* feature bits, so a 1.3 device can still lack them.
    // The bindless material table needs an unbounded, partially-bound,
    // non-uniformly-indexed sampled-image array, and the indirect-draw stages
    // need drawIndirectCount. Required here (well before D) so unsupported
    // hardware fails loudly at selection instead of mid-migration — see
    // docs/mesh-material-architecture.md §10. Must stay in lock-step with the
    // enables in CreateLogicalDevice.
    if (features12.descriptorIndexing != VK_TRUE || features12.runtimeDescriptorArray != VK_TRUE ||
        features12.shaderSampledImageArrayNonUniformIndexing != VK_TRUE ||
        features12.descriptorBindingPartiallyBound != VK_TRUE ||
        features12.descriptorBindingVariableDescriptorCount != VK_TRUE || features12.drawIndirectCount != VK_TRUE)
    {
        Core::Log::Info("  rejected: missing bindless/descriptor-indexing support "
                        "(descriptorIndexing={}, runtimeDescriptorArray={}, "
                        "shaderSampledImageArrayNonUniformIndexing={}, descriptorBindingPartiallyBound={}, "
                        "descriptorBindingVariableDescriptorCount={}, drawIndirectCount={})",
                        features12.descriptorIndexing == VK_TRUE, features12.runtimeDescriptorArray == VK_TRUE,
                        features12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE,
                        features12.descriptorBindingPartiallyBound == VK_TRUE,
                        features12.descriptorBindingVariableDescriptorCount == VK_TRUE,
                        features12.drawIndirectCount == VK_TRUE);
        return false;
    }

    // Multi-draw indirect (GPU-driven stage E). The mesh pass issues the whole
    // frame as one vkCmdDrawIndexedIndirect over a CPU-built command buffer whose
    // batches carry a non-zero firstInstance (each instanced draw starts partway
    // into the shared instance buffer) — so both the multi-draw feature and the
    // firstInstance-in-indirect feature are required. Both are Vulkan 1.0 *core*
    // but optional bits (VkPhysicalDeviceFeatures), near-universal on desktop.
    // Checked here so unsupported hardware fails at selection, not mid-frame; must
    // stay in lock-step with the enables in CreateLogicalDevice.
    if (features2.features.multiDrawIndirect != VK_TRUE || features2.features.drawIndirectFirstInstance != VK_TRUE)
    {
        Core::Log::Info("  rejected: missing indirect-draw support (multiDrawIndirect={}, "
                        "drawIndirectFirstInstance={})",
                        features2.features.multiDrawIndirect == VK_TRUE,
                        features2.features.drawIndirectFirstInstance == VK_TRUE);
        return false;
    }

    return true;
}

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

        // Selection must agree with CreateLogicalDevice's hard requirements, or a
        // capable-looking-but-unsupported device gets chosen and then fails at
        // vkCreateDevice. Skip any that can't satisfy them and keep looking.
        if (!DeviceMeetsRequirements(device, props))
        {
            continue;
        }

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

VkDevice CreateLogicalDevice(VkPhysicalDevice physicalDevice, uint32_t graphicsQueueFamily, bool enableAnisotropy)
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
    // The scene fragment shaders compile `discard` to SPIR-V DemoteToHelperInvocation
    // (DXC's default), whose capability requires this bit — without it the shader
    // modules trip a validation error. Vulkan 1.3 core; the device already requires 1.3.
    features13.shaderDemoteToHelperInvocation = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.timelineSemaphore = VK_TRUE;
    // Descriptor indexing / bindless (GPU-driven stage D). Enabled so the device
    // is created bindless-ready: an unbounded, partially-bound, non-uniformly-
    // indexed sampled-image array for the material table, plus drawIndirectCount
    // for the indirect-draw stages. DeviceMeetsRequirements already verified the
    // chosen device supports all of these, so requesting them here can't fail.
    features12.descriptorIndexing = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    features12.drawIndirectCount = VK_TRUE;
    features12.pNext = &features13;

    // Core 1.0 features. Anisotropic filtering is optional in the spec, so it is
    // only requested when the caller confirmed the device advertises it (see
    // Create()); requesting an unsupported feature fails vkCreateDevice outright.
    VkPhysicalDeviceFeatures coreFeatures{};
    coreFeatures.samplerAnisotropy = enableAnisotropy ? VK_TRUE : VK_FALSE;
    // Indirect draws (GPU-driven stage E): the mesh pass submits the frame as a
    // single multi-draw over a CPU-built command buffer whose batches use a
    // non-zero firstInstance. DeviceMeetsRequirements already verified both bits.
    coreFeatures.multiDrawIndirect = VK_TRUE;
    coreFeatures.drawIndirectFirstInstance = VK_TRUE;

    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.pNext = &features12;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount = 1;
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions;
    deviceCreateInfo.pEnabledFeatures = &coreFeatures;

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
        Core::ShowErrorDialog("Assisi — Vulkan unavailable",
                              "Could not initialize Vulkan.\n\n"
                              "Assisi renders with Vulkan 1.3 and could not create a Vulkan instance. "
                              "This usually means the graphics drivers are missing or out of date.\n\n"
                              "Please install or update your graphics drivers. See assisi.log for details.");
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
        Core::ShowErrorDialog("Assisi — Unsupported graphics device",
                              "No compatible GPU was found.\n\n"
                              "Assisi requires a graphics device with Vulkan 1.3 support "
                              "(dynamic rendering, synchronization2, and timeline semaphores).\n\n"
                              "Please update your graphics drivers. If they are already up to date, "
                              "your GPU is likely too old to run Assisi. See assisi.log for details.");
        return nullptr;
    }
    context->_physicalDevice = physicalDeviceChoice->physicalDevice;
    context->_graphicsQueueFamily = physicalDeviceChoice->graphicsQueueFamily;

    VkPhysicalDeviceProperties chosenProps{};
    VKD.vkGetPhysicalDeviceProperties(context->_physicalDevice, &chosenProps);
    Core::Log::Info("VulkanContext: selected device {}", chosenProps.deviceName);

    // Anisotropic filtering: enable it only if the device supports it, and clamp
    // the sampler request to the device's limit. 8x is a good quality/cost
    // default for texture minification at grazing angles (plain trilinear
    // over-blurs there); samplers read this back via GetMaxAnisotropy().
    constexpr float          kDesiredMaxAnisotropy = 8.0f;
    VkPhysicalDeviceFeatures supportedFeatures{};
    VKD.vkGetPhysicalDeviceFeatures(context->_physicalDevice, &supportedFeatures);
    const bool anisotropySupported = supportedFeatures.samplerAnisotropy == VK_TRUE;
    context->_maxAnisotropy =
        anisotropySupported ? std::min(kDesiredMaxAnisotropy, chosenProps.limits.maxSamplerAnisotropy) : 1.0f;

    context->_device =
        CreateLogicalDevice(context->_physicalDevice, context->_graphicsQueueFamily, anisotropySupported);
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

    // One image-available semaphore and one event query per in-flight slot. The
    // render-finished semaphores are per swapchain image and were created inside
    // CreateSwapchainResources() above.
    {
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            if (VKD.vkCreateSemaphore(context->_device, &semInfo, nullptr, &context->_imageAvailableSemaphores[i]) !=
                VK_SUCCESS)
            {
                Core::Log::Error("VulkanContext: vkCreateSemaphore failed.");
                return nullptr;
            }
            context->_frameQueries[i] = context->_nvrhiDevice->createEventQuery();
            context->_timerQueries[i] = context->_nvrhiDevice->createTimerQuery();
        }
    }

    context->_commandList = context->_nvrhiDevice->createCommandList();

    return context;
}

uint32_t VulkanContext::GetMaxUsableSampleCount() const
{
    if (_physicalDevice == VK_NULL_HANDLE)
    {
        return 1u;
    }

    VkPhysicalDeviceProperties props{};
    VKD.vkGetPhysicalDeviceProperties(_physicalDevice, &props);

    // A render target needs BOTH its colour and depth attachments at the sample
    // count, so intersect the two masks — a device can advertise more colour
    // sample counts than depth ones.
    const VkSampleCountFlags counts =
        props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;

    if (counts & VK_SAMPLE_COUNT_64_BIT) return 64u;
    if (counts & VK_SAMPLE_COUNT_32_BIT) return 32u;
    if (counts & VK_SAMPLE_COUNT_16_BIT) return 16u;
    if (counts & VK_SAMPLE_COUNT_8_BIT)  return 8u;
    if (counts & VK_SAMPLE_COUNT_4_BIT)  return 4u;
    if (counts & VK_SAMPLE_COUNT_2_BIT)  return 2u;
    return 1u;
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
    // The scene renders into the swapchain image as a colour attachment; when no
    // AA mode is active the frame also *clears* it directly (Application::OnRender
    // → clearTextureFloat), which is a transfer, so the image must additionally
    // declare TRANSFER_DST — without it that clear is a spec violation (validation
    // fires; the driver merely tolerates it). Guarded by the surface's advertised
    // usage, though every real presentation engine supports TRANSFER_DST.
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
    {
        createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
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

    // Past this point the old swapchain and its resources are gone, so we can no
    // longer "leave the previous swapchain intact" on failure. Any late step that
    // fails calls ResetToNoSwapchain() before returning, so BeginFrame's
    // `_swapchain == VK_NULL_HANDLE` guard short-circuits and a later Resize() can
    // retry cleanly, rather than BeginFrame indexing into empty vectors.
    uint32_t actualImageCount = 0;
    VKD.vkGetSwapchainImagesKHR(_device, _swapchain, &actualImageCount, nullptr);
    _swapchainImages.resize(actualImageCount);
    VKD.vkGetSwapchainImagesKHR(_device, _swapchain, &actualImageCount, _swapchainImages.data());

    // One render-finished semaphore per swapchain image (see the header note on
    // why these are per-image rather than per in-flight slot). Tied to the
    // swapchain's lifetime — DestroySwapchainResources() frees them.
    _renderFinishedSemaphores.resize(_swapchainImages.size());
    {
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (VkSemaphore &semaphore : _renderFinishedSemaphores)
        {
            if (VKD.vkCreateSemaphore(_device, &semInfo, nullptr, &semaphore) != VK_SUCCESS)
            {
                Core::Log::Error("VulkanContext: vkCreateSemaphore (render-finished) failed.");
                ResetToNoSwapchain();
                return false;
            }
        }
    }

    const nvrhi::Format colorFormat = ToNvrhiFormat(_swapchainFormat);
    if (colorFormat == nvrhi::Format::UNKNOWN)
    {
        Core::Log::Error("VulkanContext: swapchain format {} has no NVRHI mapping.",
                         static_cast<int32_t>(_swapchainFormat));
        ResetToNoSwapchain();
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
        ResetToNoSwapchain();
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

    // Callers wait for the device to go idle before recreating the swapchain, so
    // no present is still referencing these.
    for (VkSemaphore &semaphore : _renderFinishedSemaphores)
    {
        if (semaphore != VK_NULL_HANDLE)
            VKD.vkDestroySemaphore(_device, semaphore, nullptr);
    }
    _renderFinishedSemaphores.clear();
}

void VulkanContext::ResetToNoSwapchain()
{
    DestroySwapchainResources();
    if (_swapchain != VK_NULL_HANDLE)
    {
        VKD.vkDestroySwapchainKHR(_device, _swapchain, nullptr);
        _swapchain = VK_NULL_HANDLE;
    }
}

void VulkanContext::Resize(uint32_t width, uint32_t height)
{
    if (_device == VK_NULL_HANDLE)
    {
        return;
    }

    VKD.vkDeviceWaitIdle(_device);

    // A failure here is benign and intentionally not propagated. A zero-size
    // (minimized) window, or a failure before the old swapchain is torn down,
    // leaves the previous swapchain intact. A late failure (after teardown)
    // instead leaves us in a consistent no-swapchain state (_swapchain ==
    // VK_NULL_HANDLE). Either way BeginFrame() short-circuits and a later resize
    // recovers. Explicitly discarded to satisfy [[nodiscard]].
    (void)CreateSwapchainResources(width, height);
}

VkPresentModeKHR VulkanContext::ChoosePresentMode()
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
    // Record the fallback so IsVSyncEnabled() reports what the swapchain
    // actually does — pacing decisions read this flag.
    _vsyncEnabled = true;
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
    // same as Resize(): either the old swapchain survives (early failure) or we end
    // up in a consistent no-swapchain state (late failure) that a resize recovers.
    (void)CreateSwapchainResources(_swapchainExtent.width, _swapchainExtent.height);
}

std::optional<RenderFrame> VulkanContext::BeginFrame()
{
    // A same-size OUT_OF_DATE (display-mode change, monitor hot-plug, compositor
    // restart) doesn't change the framebuffer dimensions, so the window resize
    // callback never fires and Resize() is never called — without this, rendering
    // would freeze forever. Rebuild here at the current surface extent
    // (CreateSwapchainResources re-queries capabilities.currentExtent, so the
    // passed extent is only the Wayland/no-currentExtent fallback). Keep the flag
    // set until a rebuild actually succeeds, so a transient failure (e.g. a frame
    // caught mid-minimize with a zero extent) simply retries next frame instead of
    // leaving us wedged with a stale swapchain.
    if (_swapchainStale && _device != VK_NULL_HANDLE)
    {
        VKD.vkDeviceWaitIdle(_device);
        (void)CreateSwapchainResources(_swapchainExtent.width, _swapchainExtent.height);
        if (_swapchain != VK_NULL_HANDLE)
        {
            _swapchainStale = false;
        }
    }

    if (_swapchain == VK_NULL_HANDLE)
    {
        return std::nullopt; // minimized; Resize() hasn't (re)created anything yet
    }

    const uint32_t slot = static_cast<uint32_t>(_frameCounter % kFramesInFlight);

    // Throttle to kFramesInFlight: block until the frame that last used THIS slot
    // has finished on the GPU — and only that frame, so the other in-flight frame
    // keeps executing (the whole point, vs. the old full waitForIdle()). This
    // gates reuse of the slot's image-available semaphore and the ImGui buffers
    // that frame round-robined. Guarded by a per-slot flag so an early-out below
    // (stale swapchain) doesn't leave a query waited-but-never-reset.
    _lastGpuWaitMs = 0.0;
    if (_frameQueryPending[slot])
    {
        const std::chrono::steady_clock::time_point waitStart = std::chrono::steady_clock::now();
        _nvrhiDevice->waitEventQuery(_frameQueries[slot]);
        _lastGpuWaitMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
        _nvrhiDevice->resetEventQuery(_frameQueries[slot]);

        // The event query just proved this slot's previous frame is done on the
        // GPU, so its timer query is already resolved — this read won't block.
        // getTimerQueryTime() clears the query's "started" flag, so the
        // beginTimerQuery() below can reuse it.
        _lastGpuFrameMs = _nvrhiDevice->getTimerQueryTime(_timerQueries[slot]) * 1000.0f;
        _frameQueryPending[slot] = false;
    }

    // vkAcquireNextImageKHR blocks the CPU until the presentation engine hands
    // back a swapchain image; under vsync/back-pressure that stall lands here (or
    // in submit/present below). Fold it into _lastGpuWaitMs so it's excluded from
    // the CPU frame-time figure rather than mislabeled as CPU work.
    const std::chrono::steady_clock::time_point acquireStart = std::chrono::steady_clock::now();
    VkResult acquireResult = VKD.vkAcquireNextImageKHR(_device, _swapchain, UINT64_MAX, _imageAvailableSemaphores[slot],
                                                        VK_NULL_HANDLE, &_currentImageIndex);
    _lastGpuWaitMs +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - acquireStart).count();
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        // Swapchain is stale. A resize would recreate it via the window callback,
        // but a same-size stale event won't — so flag it and let the top of the
        // next BeginFrame rebuild unconditionally. Skip rendering this frame.
        _swapchainStale = true;
        return std::nullopt;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
    {
        Core::Log::Error("VulkanContext: vkAcquireNextImageKHR failed with VkResult {}",
                          static_cast<int32_t>(acquireResult));
        return std::nullopt;
    }

    _commandList->open();
    _commandList->beginTimerQuery(_timerQueries[slot]); // spans the whole frame; ended in EndFrame()
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
    // Same slot BeginFrame() used — _frameCounter isn't advanced until the end of
    // this function, and EndFrame() only runs when BeginFrame() returned a frame.
    const uint32_t slot = static_cast<uint32_t>(_frameCounter % kFramesInFlight);

    // Submit + present block the CPU on the graphics queue / presentation engine;
    // like the acquire above, that stall is idle-waiting on the GPU/display, not
    // CPU work, so time it and fold it into _lastGpuWaitMs. GC (below, after this
    // window) is genuine CPU work and stays counted.
    const std::chrono::steady_clock::time_point presentWaitStart = std::chrono::steady_clock::now();

    _commandList->endTimerQuery(_timerQueries[slot]); // paired with beginTimerQuery in BeginFrame()
    _commandList->close();

    _nvrhiDevice->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, _imageAvailableSemaphores[slot], 0);
    _nvrhiDevice->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, _renderFinishedSemaphores[_currentImageIndex], 0);
    _nvrhiDevice->executeCommandList(_commandList);

    // Snapshot this submission's completion into the slot's query; the frame that
    // reuses this slot kFramesInFlight later waits on it in BeginFrame().
    _nvrhiDevice->setEventQuery(_frameQueries[slot], nvrhi::CommandQueue::Graphics);
    _frameQueryPending[slot] = true;

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &_renderFinishedSemaphores[_currentImageIndex];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &_swapchain;
    presentInfo.pImageIndices = &_currentImageIndex;
    const VkResult presentResult = VKD.vkQueuePresentKHR(_graphicsQueue, &presentInfo);
    _lastGpuWaitMs +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - presentWaitStart).count();
    // OUT_OF_DATE/SUBOPTIMAL are expected on resize; they're not errors here.
    // OUT_OF_DATE means the swapchain must be rebuilt — flag it so the next
    // BeginFrame does so even when no resize callback follows (same-size event).
    // SUBOPTIMAL still presents correctly, so we leave it be to avoid a rebuild
    // loop on drivers that report it persistently. Anything else is a real error.
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        _swapchainStale = true;
    }
    else if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR)
    {
        Core::Log::Error("VulkanContext: vkQueuePresentKHR failed with VkResult {}",
                         static_cast<int32_t>(presentResult));
    }

    _nvrhiDevice->runGarbageCollection();
    ++_frameCounter;
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
        for (nvrhi::EventQueryHandle &query : _frameQueries)
            query = nullptr;
        for (nvrhi::TimerQueryHandle &query : _timerQueries)
            query = nullptr;
        DestroySwapchainResources(); // also frees the per-image render-finished semaphores
        _nvrhiDevice = nullptr;
        _nvrhiDeviceHandle = nullptr;

        for (VkSemaphore &semaphore : _imageAvailableSemaphores)
        {
            if (semaphore != VK_NULL_HANDLE) VKD.vkDestroySemaphore(_device, semaphore, nullptr);
        }

        if (_swapchain != VK_NULL_HANDLE) VKD.vkDestroySwapchainKHR(_device, _swapchain, nullptr);
        VKD.vkDestroyDevice(_device, nullptr);
    }

    if (_surface != VK_NULL_HANDLE) VKD.vkDestroySurfaceKHR(_instance, _surface, nullptr);

    if (_debugMessenger != VK_NULL_HANDLE)
        VKD.vkDestroyDebugUtilsMessengerEXT(_instance, _debugMessenger, nullptr);

    if (_instance != VK_NULL_HANDLE) VKD.vkDestroyInstance(_instance, nullptr);
}

} // namespace Assisi::Render::Vulkan
