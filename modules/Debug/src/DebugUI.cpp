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
#include <implot.h>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Debug/DebugUI.hpp>
#include <Assisi/Render/ShaderModule.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>

#define VKD VULKAN_HPP_DEFAULT_DISPATCHER

namespace Assisi::Debug
{

namespace
{
VkDevice s_vkDevice = VK_NULL_HANDLE;
VkDescriptorPool s_descriptorPool = VK_NULL_HANDLE;

// Descriptor-set budget for user textures (asset-browser thumbnails, etc.). The
// per-frame mark-and-sweep below keeps the live set bounded to what is actually
// requested each frame — roughly one directory of thumbnails — so this is a
// generous ceiling, not a per-session accumulator like the old maxSets=16.
constexpr std::uint32_t kMaxDebugTextures = 256;

// A texture's descriptor set can be referenced by in-flight command buffers for up
// to the renderer's frames-in-flight depth (VulkanContext::kFramesInFlight == 2).
// Retire a badge only after it has gone unused for at least that many frames, so
// ImGui_ImplVulkan_RemoveTexture's vkFreeDescriptorSets never frees a set the GPU
// is still reading. One extra frame of slack for safety.
constexpr std::uint64_t kTextureRetireDelayFrames = 3;

// ImGui descriptor sets ("badges") registered for user textures (see
// GetOrCreateTextureId), keyed by the NVRHI texture they wrap, each tagged with the
// frame it was last requested. Badges unused for kTextureRetireDelayFrames frames
// are recycled in BeginFrame; any survivors are freed wholesale when
// s_descriptorPool is destroyed in Shutdown().
struct RegisteredTexture
{
    VkDescriptorSet set           = VK_NULL_HANDLE;
    std::uint64_t   lastUsedFrame = 0;
};
using TextureIdMap = std::unordered_map<nvrhi::ITexture *, RegisteredTexture>;
TextureIdMap s_textureIds;

// Monotonic frame counter advanced by BeginFrame; drives badge retirement.
std::uint64_t s_frameIndex = 0;

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

// Free the descriptor sets of textures not requested within the last
// kTextureRetireDelayFrames frames. Called from BeginFrame, where the deferral
// guarantees every command buffer that referenced the set has finished, so
// vkFreeDescriptorSets (via ImGui_ImplVulkan_RemoveTexture) is safe.
void SweepRetiredTextures()
{
    for (TextureIdMap::iterator it = s_textureIds.begin(); it != s_textureIds.end();)
    {
        if (s_frameIndex - it->second.lastUsedFrame >= kTextureRetireDelayFrames)
        {
            ImGui_ImplVulkan_RemoveTexture(it->second.set);
            it = s_textureIds.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
} // namespace

void DebugUI::Initialize(const Window::WindowContext &window, Render::Vulkan::VulkanContext &vulkanContext)
{
    s_vkDevice = vulkanContext.GetVkDevice();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

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
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxDebugTextures},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kMaxDebugTextures},
    }};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    // FREE_DESCRIPTOR_SET_BIT lets SweepRetiredTextures/RemoveTexture recycle
    // individual sets (vkFreeDescriptorSets) rather than only pool-wide resets.
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = kMaxDebugTextures;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    VKD.vkCreateDescriptorPool(s_vkDevice, &poolInfo, nullptr, &s_descriptorPool);

    ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_2, LoadVulkanFunction,
                                   reinterpret_cast<void *>(vulkanContext.GetVkInstance()));

    // Match the depth format VulkanContext actually chose for the swapchain
    // (D24S8 -> D32S8 -> D32, device-dependent). Only advertise a stencil
    // aspect when the chosen format carries one — D32_SFLOAT does not.
    s_colorFormat = vulkanContext.GetSwapchainFormat();
    const VkFormat kDepthFormat   = vulkanContext.GetDepthFormat();
    const bool     kDepthHasStencil = (kDepthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
                                       kDepthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT);

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
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.stencilAttachmentFormat =
        kDepthHasStencil ? kDepthFormat : VK_FORMAT_UNDEFINED;
    initInfo.CheckVkResultFn = CheckVkResult;

    ImGui_ImplVulkan_Init(&initInfo);

    nvrhi::IDevice *device = vulkanContext.GetDevice();
    const nvrhi::ShaderHandle vertexShader =
        Render::LoadSpirvShader(device, "shaders/imgui_opener.vert.spv", nvrhi::ShaderType::Vertex);
    const nvrhi::ShaderHandle pixelShader =
        Render::LoadSpirvShader(device, "shaders/imgui_opener.frag.spv", nvrhi::ShaderType::Pixel);

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
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    s_openerPipeline = nullptr;

    // Destroying the pool frees every set ImGui_ImplVulkan_AddTexture handed out;
    // drop our bookkeeping to match.
    s_textureIds.clear();
    if (s_descriptorPool != VK_NULL_HANDLE)
    {
        VKD.vkDestroyDescriptorPool(s_vkDevice, s_descriptorPool, nullptr);
        s_descriptorPool = VK_NULL_HANDLE;
    }
}

void DebugUI::BeginFrame(Render::RenderFrame &frame)
{
    // Advance the frame clock and reclaim badges no window has drawn recently. Runs
    // every frame regardless of what's open, so closing the asset browser (or
    // leaving a directory) frees that directory's thumbnails a few frames later.
    ++s_frameIndex;
    SweepRetiredTextures();

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

ImTextureID DebugUI::GetOrCreateTextureId(nvrhi::ITexture *texture)
{
    if (texture == nullptr)
        return ImTextureID_Invalid;

    if (TextureIdMap::iterator it = s_textureIds.find(texture); it != s_textureIds.end())
    {
        it->second.lastUsedFrame = s_frameIndex; // mark: still in use this frame
        return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(it->second.set));
    }

    // NVRHI doesn't surface image views through getNativeObject (that's only the
    // VkImage) — views are created per-format/subresource on demand via
    // getNativeView. Ask for the whole-resource SRV; the defaults resolve to the
    // texture's own format and dimension.
    VkImageView imageView = texture->getNativeView(nvrhi::ObjectTypes::VK_ImageView);
    if (imageView == VK_NULL_HANDLE)
    {
        Assisi::Core::Log::Error("DebugUI: texture exposes no VkImageView; cannot display it in ImGui.");
        return ImTextureID_Invalid;
    }

    const VkDescriptorSet set =
        ImGui_ImplVulkan_AddTexture(imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    s_textureIds.emplace(texture, RegisteredTexture{set, s_frameIndex});
    return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(set));
}

void DebugUI::EndFrame(Render::RenderFrame &frame)
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
