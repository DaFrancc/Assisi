/// @file main.cpp
/// @brief Standalone NVRHI + Vulkan bring-up test: clears the screen and draws
/// one hardcoded triangle. Not wired into Assisi::App — this is a throwaway
/// spike to prove the Vulkan/NVRHI path works before it gets folded into the
/// real Render module.
///
/// There is no Vulkan SDK installed in this environment (driver-only), so
/// there is no vulkan-1.lib to link against. Instead, exactly like NVRHI's
/// own reference framework (Donut's DeviceManager_VK.cpp) does, this uses
/// vulkan.hpp's dynamic dispatcher: a vk::detail::DynamicLoader pulls
/// vkGetInstanceProcAddr out of the driver's vulkan-1.dll at runtime, and
/// every Vulkan call in this file goes through that same global dispatcher
/// (VULKAN_HPP_DEFAULT_DISPATCHER, aliased below as VKD) instead of a
/// statically-linked symbol. NVRHI's own Vulkan backend was built expecting
/// exactly this dispatcher to exist and be initialized by the app.

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include <GLFW/glfw3.h>

#include <nvrhi/nvrhi.h>
#include <nvrhi/vulkan.h>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Sinks.hpp>
#include <Assisi/Window/WindowContext.hpp>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <vector>

#define VKD VULKAN_HPP_DEFAULT_DISPATCHER

using namespace Assisi;

namespace
{
constexpr uint32_t kFramesInFlight = 2;

std::vector<char> ReadFile(const std::string &path)
{
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open())
    {
        Core::Log::Fatal("Failed to open file: {}", path);
        std::exit(EXIT_FAILURE);
    }

    const size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(size));
    return buffer;
}

void VkCheck(VkResult result, const char *what)
{
    if (result != VK_SUCCESS)
    {
        Core::Log::Fatal("{} failed with VkResult {}", what, static_cast<int>(result));
        std::exit(EXIT_FAILURE);
    }
}

// --- Instance -----------------------------------------------------------

VkInstance CreateInstance()
{
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Assisi VkTriangle";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "Assisi";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    uint32_t glfwExtensionCount = 0;
    const char **glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = glfwExtensionCount;
    createInfo.ppEnabledExtensionNames = glfwExtensions;

    VkInstance instance = VK_NULL_HANDLE;
    VkCheck(VKD.vkCreateInstance(&createInfo, nullptr, &instance), "vkCreateInstance");
    return instance;
}

// --- Physical device / queue selection -----------------------------------

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

        // Prefer the first discrete GPU found; otherwise fall back to whatever's usable.
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

// --- Logical device -------------------------------------------------------

VkDevice CreateLogicalDevice(VkPhysicalDevice physicalDevice, uint32_t graphicsQueueFamily)
{
    const float queuePriority = 1.0f;

    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = graphicsQueueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    const char *deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount = 1;
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions;

    VkDevice device = VK_NULL_HANDLE;
    VkCheck(VKD.vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device), "vkCreateDevice");
    return device;
}

// --- Swapchain --------------------------------------------------------------

struct Swapchain
{
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
};

Swapchain CreateSwapchain(VkPhysicalDevice physicalDevice, VkDevice device, VkSurfaceKHR surface,
                          uint32_t graphicsQueueFamily, const Window::WindowSize &framebufferSize)
{
    VkSurfaceCapabilitiesKHR capabilities{};
    VKD.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);

    uint32_t formatCount = 0;
    VKD.vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    VKD.vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());

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
        extent.width = static_cast<uint32_t>(framebufferSize.Width);
        extent.height = static_cast<uint32_t>(framebufferSize.Height);
    }

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount)
    {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = chosenFormat.format;
    createInfo.imageColorSpace = chosenFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR; // always available, effectively vsync
    createInfo.clipped = VK_TRUE;
    (void)graphicsQueueFamily; // single queue family owns both graphics + present here

    Swapchain result;
    result.format = chosenFormat.format;
    result.extent = extent;
    VkCheck(VKD.vkCreateSwapchainKHR(device, &createInfo, nullptr, &result.handle), "vkCreateSwapchainKHR");

    uint32_t actualImageCount = 0;
    VKD.vkGetSwapchainImagesKHR(device, result.handle, &actualImageCount, nullptr);
    result.images.resize(actualImageCount);
    VKD.vkGetSwapchainImagesKHR(device, result.handle, &actualImageCount, result.images.data());

    return result;
}

nvrhi::Format ToNvrhiFormat(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_B8G8R8A8_UNORM: return nvrhi::Format::BGRA8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:  return nvrhi::Format::SBGRA8_UNORM;
    default:
        Core::Log::Fatal("Unhandled VkFormat {} for swapchain", static_cast<int>(format));
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main()
{
    Core::GetLogger().AddSink(std::make_shared<Core::ConsoleSink>());
    Core::Log::Info("Assisi VkTriangle starting up.");

    // Bootstrap the dynamic dispatcher: load vulkan-1.dll ourselves and resolve
    // vkGetInstanceProcAddr from it, since there's no SDK import lib to link.
    vk::detail::DynamicLoader dynamicLoader;
    auto vkGetInstanceProcAddr = dynamicLoader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    VKD.init(vkGetInstanceProcAddr);

    Window::WindowConfiguration windowConfig;
    windowConfig.Width = 1280;
    windowConfig.Height = 720;
    windowConfig.Title = "Assisi VkTriangle";

    Window::WindowContext window(windowConfig, nullptr);
    if (!window.IsValid())
    {
        Core::Log::Fatal("Failed to create window.");
        return EXIT_FAILURE;
    }

    VkInstance instance = CreateInstance();
    VKD.init(vk::Instance(instance)); // resolves the rest of the instance-level functions

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkCheck(glfwCreateWindowSurface(instance, window.NativeHandle(), nullptr, &surface),
            "glfwCreateWindowSurface");

    auto physicalDeviceChoice = ChoosePhysicalDevice(instance, surface);
    if (!physicalDeviceChoice.has_value())
    {
        Core::Log::Fatal("No suitable Vulkan physical device found.");
        return EXIT_FAILURE;
    }

    VkPhysicalDevice physicalDevice = physicalDeviceChoice->physicalDevice;
    uint32_t graphicsQueueFamily = physicalDeviceChoice->graphicsQueueFamily;

    VkPhysicalDeviceProperties chosenProps{};
    VKD.vkGetPhysicalDeviceProperties(physicalDevice, &chosenProps);
    Core::Log::Info("Selected Vulkan device: {}", chosenProps.deviceName);

    VkDevice device = CreateLogicalDevice(physicalDevice, graphicsQueueFamily);
    VKD.init(vk::Device(device)); // resolves device-level functions (swapchain, queue submission, ...)

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VKD.vkGetDeviceQueue(device, graphicsQueueFamily, 0, &graphicsQueue);

    // --- NVRHI device wrapping the Vulkan objects we just created ---------
    const char *deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    nvrhi::vulkan::DeviceDesc nvrhiDeviceDesc;
    nvrhiDeviceDesc.instance = instance;
    nvrhiDeviceDesc.physicalDevice = physicalDevice;
    nvrhiDeviceDesc.device = device;
    nvrhiDeviceDesc.graphicsQueue = graphicsQueue;
    nvrhiDeviceDesc.graphicsQueueIndex = static_cast<int>(graphicsQueueFamily);
    nvrhiDeviceDesc.deviceExtensions = deviceExtensions;
    nvrhiDeviceDesc.numDeviceExtensions = 1;

    nvrhi::vulkan::DeviceHandle nvrhiDevice = nvrhi::vulkan::createDevice(nvrhiDeviceDesc);
    if (!nvrhiDevice)
    {
        Core::Log::Fatal("nvrhi::vulkan::createDevice failed.");
        return EXIT_FAILURE;
    }

    // --- Swapchain and per-image NVRHI textures/framebuffers ----------------
    Swapchain swapchain =
        CreateSwapchain(physicalDevice, device, surface, graphicsQueueFamily, window.GetFramebufferSize());
    const nvrhi::Format colorFormat = ToNvrhiFormat(swapchain.format);

    std::vector<nvrhi::TextureHandle> swapchainTextures;
    std::vector<nvrhi::FramebufferHandle> framebuffers;

    for (VkImage image : swapchain.images)
    {
        nvrhi::TextureDesc textureDesc;
        textureDesc.width = swapchain.extent.width;
        textureDesc.height = swapchain.extent.height;
        textureDesc.format = colorFormat;
        textureDesc.isRenderTarget = true;
        textureDesc.debugName = "SwapchainImage";
        textureDesc.enableAutomaticStateTracking(nvrhi::ResourceStates::Present);

        nvrhi::TextureHandle texture =
            nvrhiDevice->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image, nvrhi::Object(image), textureDesc);
        swapchainTextures.push_back(texture);

        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(texture);
        framebuffers.push_back(nvrhiDevice->createFramebuffer(fbDesc));
    }

    // --- Shaders (compiled to SPIR-V at build time by CMake) ---------------
    std::vector<char> vertSpv = ReadFile("shaders/triangle.vert.spv");
    std::vector<char> fragSpv = ReadFile("shaders/triangle.frag.spv");

    nvrhi::ShaderDesc vertDesc;
    vertDesc.shaderType = nvrhi::ShaderType::Vertex;
    vertDesc.debugName = "triangle.vert";
    nvrhi::ShaderHandle vertexShader = nvrhiDevice->createShader(vertDesc, vertSpv.data(), vertSpv.size());

    nvrhi::ShaderDesc fragDesc;
    fragDesc.shaderType = nvrhi::ShaderType::Pixel;
    fragDesc.debugName = "triangle.frag";
    nvrhi::ShaderHandle fragmentShader = nvrhiDevice->createShader(fragDesc, fragSpv.data(), fragSpv.size());

    // --- Pipeline -------------------------------------------------------------
    nvrhi::FramebufferInfo fbInfo = framebuffers[0]->getFramebufferInfo();

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.VS = vertexShader;
    pipelineDesc.PS = fragmentShader;
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;

    nvrhi::GraphicsPipelineHandle pipeline = nvrhiDevice->createGraphicsPipeline(pipelineDesc, fbInfo);

    // --- Per-frame sync + command list ----------------------------------------
    std::vector<VkSemaphore> imageAvailableSemaphores(kFramesInFlight);
    std::vector<VkSemaphore> renderFinishedSemaphores(kFramesInFlight);
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkCheck(VKD.vkCreateSemaphore(device, &semInfo, nullptr, &imageAvailableSemaphores[i]), "vkCreateSemaphore");
        VkCheck(VKD.vkCreateSemaphore(device, &semInfo, nullptr, &renderFinishedSemaphores[i]), "vkCreateSemaphore");
    }

    nvrhi::CommandListHandle commandList = nvrhiDevice->createCommandList();

    Core::Log::Info("Setup complete, entering render loop.");

    uint32_t frameIndex = 0;
    while (!window.ShouldClose())
    {
        Window::WindowContext::PollEvents();

        VkSemaphore imageAvailable = imageAvailableSemaphores[frameIndex % kFramesInFlight];
        VkSemaphore renderFinished = renderFinishedSemaphores[frameIndex % kFramesInFlight];

        uint32_t imageIndex = 0;
        VkResult acquireResult = VKD.vkAcquireNextImageKHR(device, swapchain.handle, UINT64_MAX, imageAvailable,
                                                            VK_NULL_HANDLE, &imageIndex);
        if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
        {
            Core::Log::Error("vkAcquireNextImageKHR failed with VkResult {}; window resize handling is not "
                              "implemented in this bring-up test.",
                              static_cast<int>(acquireResult));
            break;
        }

        commandList->open();
        commandList->setTextureState(swapchainTextures[imageIndex], nvrhi::AllSubresources,
                                      nvrhi::ResourceStates::RenderTarget);
        commandList->clearTextureFloat(swapchainTextures[imageIndex], nvrhi::AllSubresources,
                                        nvrhi::Color(0.05f, 0.05f, 0.08f, 1.0f));

        nvrhi::GraphicsState state;
        state.pipeline = pipeline;
        state.framebuffer = framebuffers[imageIndex];
        state.viewport.addViewportAndScissorRect(
            nvrhi::Viewport(static_cast<float>(swapchain.extent.width), static_cast<float>(swapchain.extent.height)));
        commandList->setGraphicsState(state);

        nvrhi::DrawArguments drawArgs;
        drawArgs.vertexCount = 3;
        commandList->draw(drawArgs);

        commandList->close();

        nvrhiDevice->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, imageAvailable, 0);
        nvrhiDevice->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, renderFinished, 0);
        nvrhiDevice->executeCommandList(commandList);

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinished;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain.handle;
        presentInfo.pImageIndices = &imageIndex;
        VKD.vkQueuePresentKHR(graphicsQueue, &presentInfo);

        nvrhiDevice->runGarbageCollection();
        ++frameIndex;
    }

    Core::Log::Info("Shutting down.");
    VKD.vkDeviceWaitIdle(device);

    commandList = nullptr;
    pipeline = nullptr;
    for (auto &fb : framebuffers) fb = nullptr;
    for (auto &tex : swapchainTextures) tex = nullptr;
    vertexShader = nullptr;
    fragmentShader = nullptr;
    nvrhiDevice = nullptr;

    for (VkSemaphore sem : imageAvailableSemaphores) VKD.vkDestroySemaphore(device, sem, nullptr);
    for (VkSemaphore sem : renderFinishedSemaphores) VKD.vkDestroySemaphore(device, sem, nullptr);
    VKD.vkDestroySwapchainKHR(device, swapchain.handle, nullptr);
    VKD.vkDestroyDevice(device, nullptr);
    VKD.vkDestroySurfaceKHR(instance, surface, nullptr);
    VKD.vkDestroyInstance(instance, nullptr);

    return EXIT_SUCCESS;
}
