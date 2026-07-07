/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

// No Vulkan SDK in this dev environment (driver runtime only, see
// VulkanContext.cpp's header comment) — route Vulkan calls through the same
// global dynamic dispatcher VulkanContext.cpp bootstraps, and load
// imgui_impl_vulkan's own function pointers the same way (no static import lib
// to link against). IMGUI_IMPL_VULKAN_NO_PROTOTYPES is set target-wide via
// CMake (Debug/CMakeLists.txt) so it also applies to imgui_impl_vulkan.cpp.
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Debug/DebugUI.hpp>
#include <Assisi/Render/ShaderModule.hpp>

#include <algorithm>
#include <array>

#define VKD VULKAN_HPP_DEFAULT_DISPATCHER

namespace Assisi::Debug
{

namespace
{
VkDevice s_vkDevice = VK_NULL_HANDLE;
VkDescriptorPool s_descriptorPool = VK_NULL_HANDLE;

// The "opener" pipeline: never actually drawn with (see imgui_opener.vert's
// comment). Its only job is to make DebugUI::BeginFrame's setGraphicsState
// call open the frame's render target through NVRHI's own tracked path
// (NVRHI's Vulkan backend only calls vkCmdBeginRendering from inside
// setGraphicsState) so there's always something bound for ImGui to draw into
// — including when the scene has nothing to draw this frame.
nvrhi::GraphicsPipelineHandle s_openerPipeline;

// Kept alive for VkPipelineRenderingCreateInfoKHR::pColorAttachmentFormats,
// which must outlive ImGui_ImplVulkan_Init.
VkFormat s_colorFormat = VK_FORMAT_UNDEFINED;

PFN_vkVoidFunction LoadVulkanFunction(const char *functionName, void *userData)
{
    const VkInstance instance = static_cast<VkInstance>(userData);
    return VKD.vkGetInstanceProcAddr(instance, functionName);
}

void CheckVkResult(VkResult err)
{
    if (err != VK_SUCCESS)
    {
        Assisi::Core::Log::Error("DebugUI: Vulkan call failed with VkResult {}", static_cast<int>(err));
    }
}
} // namespace

void DebugUI::Initialize(const Window::WindowContext &window, Render::Vulkan::VulkanContext &vulkanContext)
{
    s_vkDevice = vulkanContext.GetVkDevice();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // No ImGuiConfigFlags_ViewportsEnable: floating ImGui windows outside the
    // main window would need per-viewport swapchains managed by the Vulkan
    // backend — out of scope for now.
    io.ConfigDragClickToInputText = true;

    ImGui::StyleColorsDark();

    auto *nativeWindow = static_cast<GLFWwindow *>(window.NativeHandle());
    ImGui_ImplGlfw_InitForVulkan(nativeWindow, true);

    const std::array<VkDescriptorPoolSize, 2> poolSizes{{
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 16},
    }};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 16;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    VKD.vkCreateDescriptorPool(s_vkDevice, &poolInfo, nullptr, &s_descriptorPool);

    ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_2, LoadVulkanFunction,
                                   reinterpret_cast<void *>(vulkanContext.GetVkInstance()));

    // D24S8 matches VulkanContext's hardcoded depth format (VulkanContext.cpp,
    // CreateSwapchainResources) — has a stencil component, hence both fields.
    s_colorFormat = vulkanContext.GetSwapchainFormat();
    constexpr VkFormat kDepthFormat = VK_FORMAT_D24_UNORM_S8_UINT;

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_2;
    initInfo.Instance = vulkanContext.GetVkInstance();
    initInfo.PhysicalDevice = vulkanContext.GetVkPhysicalDevice();
    initInfo.Device = s_vkDevice;
    initInfo.QueueFamily = vulkanContext.GetVkGraphicsQueueFamily();
    initInfo.Queue = vulkanContext.GetVkGraphicsQueue();
    initInfo.DescriptorPool = s_descriptorPool;
    initInfo.MinImageCount = std::max(2u, vulkanContext.GetSwapchainImageCount());
    initInfo.ImageCount = vulkanContext.GetSwapchainImageCount();
    initInfo.UseDynamicRendering = true;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &s_colorFormat;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat = kDepthFormat;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.stencilAttachmentFormat = kDepthFormat;
    initInfo.CheckVkResultFn = CheckVkResult;

    ImGui_ImplVulkan_Init(&initInfo);

    nvrhi::IDevice *device = vulkanContext.GetDevice();
    const nvrhi::ShaderHandle vertexShader =
        Render::CompileGlslShader(device, "shaders/imgui_opener.vert", nvrhi::ShaderType::Vertex);
    const nvrhi::ShaderHandle pixelShader =
        Render::CompileGlslShader(device, "shaders/imgui_opener.frag", nvrhi::ShaderType::Pixel);

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.VS = vertexShader;
    pipelineDesc.PS = pixelShader;
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    s_openerPipeline = device->createGraphicsPipeline(pipelineDesc, vulkanContext.GetFramebufferInfo());
    if (!s_openerPipeline)
    {
        Assisi::Core::Log::Error("DebugUI: failed to build the opener pipeline — ImGui may not render.");
    }
}

void DebugUI::Shutdown()
{
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    s_openerPipeline = nullptr;

    if (s_descriptorPool != VK_NULL_HANDLE)
    {
        VKD.vkDestroyDescriptorPool(s_vkDevice, s_descriptorPool, nullptr);
        s_descriptorPool = VK_NULL_HANDLE;
    }
}

void DebugUI::BeginFrame(Render::Vulkan::VulkanFrame &frame)
{
    if (s_openerPipeline)
    {
        nvrhi::GraphicsState state;
        state.pipeline = s_openerPipeline;
        state.framebuffer = frame.framebuffer;
        state.viewport.addViewportAndScissorRect(
            nvrhi::Viewport(static_cast<float>(frame.width), static_cast<float>(frame.height)));
        frame.commandList->setGraphicsState(state);
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void DebugUI::EndFrame(Render::Vulkan::VulkanFrame &frame)
{
    ImGui::Render();

    VkCommandBuffer commandBuffer = frame.commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);

    // Recording raw Vulkan commands outside NVRHI leaves its state cache stale
    // — reset it before any further NVRHI recording (see ICommandList::open's
    // sibling clearState() doc comment in nvrhi.h).
    frame.commandList->clearState();
}

} // namespace Assisi::Debug
