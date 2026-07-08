/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/PostProcess.hpp>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ShaderModule.hpp>
#include <Assisi/Render/Vulkan/VulkanContext.hpp>

namespace Assisi::Render
{

bool PostProcess::Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &swapchainFramebufferInfo,
                             const std::string &vertexShaderSpvPath, const std::string &fragmentShaderSpvPath)
{
    _device = device;
    _swapchainInfo = swapchainFramebufferInfo;

    const nvrhi::ShaderHandle vertexShader = LoadSpirvShader(device, vertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    const nvrhi::ShaderHandle fragmentShader =
        LoadSpirvShader(device, fragmentShaderSpvPath, nvrhi::ShaderType::Pixel);
    if (!vertexShader || !fragmentShader)
    {
        return false;
    }

    // Only the fragment shader has bindings (a fullscreen triangle needs no
    // vertex buffer or per-vertex bindings) — see fullscreen.vert/fxaa.frag.
    nvrhi::BindingLayoutDesc bindingLayoutDesc;
    bindingLayoutDesc.visibility = nvrhi::ShaderType::Pixel;
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(glm::vec2)));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    _fxaaBindingLayout = device->createBindingLayout(bindingLayoutDesc);

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true);
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    _fxaaSampler = device->createSampler(samplerDesc);

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.VS = vertexShader;
    pipelineDesc.PS = fragmentShader;
    pipelineDesc.addBindingLayout(_fxaaBindingLayout);
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;

    // FXAA always draws its final output into the real swapchain framebuffer
    // (see Resolve()), regardless of what fed it — so its pipeline is built
    // against the swapchain's FramebufferInfo, not whatever offscreen target
    // produced its input.
    _fxaaPipeline = device->createGraphicsPipeline(pipelineDesc, _swapchainInfo);
    return _fxaaPipeline != nullptr;
}

void PostProcess::Configure(uint32_t width, uint32_t height, AaMode mode, uint32_t msaaSamples)
{
    if (width == 0 || height == 0)
    {
        return; // minimized — keep whatever targets already exist
    }
    if (_width == width && _height == height && _mode == mode && _msaaSamples == msaaSamples)
    {
        return; // nothing that affects offscreen targets has changed
    }

    _width = width;
    _height = height;
    _mode = mode;
    _msaaSamples = msaaSamples;
    Rebuild();
}

void PostProcess::Rebuild()
{
    _msaaColor = nullptr;
    _msaaDepth = nullptr;
    _msaaFramebuffer = nullptr;
    _sceneColor = nullptr;
    _sceneDepth = nullptr;
    _sceneFramebuffer = nullptr;
    _fxaaBindingSet = nullptr;

    const bool needsMsaa = (_mode == AaMode::MSAA || _mode == AaMode::MSAA_FXAA);
    const bool needsScene = (_mode == AaMode::FXAA || _mode == AaMode::MSAA_FXAA);

    const nvrhi::Format colorFormat =
        _swapchainInfo.colorFormats.empty() ? nvrhi::Format::UNKNOWN : _swapchainInfo.colorFormats[0];
    const nvrhi::Format depthFormat = _swapchainInfo.depthFormat;

    if (needsMsaa)
    {
        nvrhi::TextureDesc colorDesc;
        colorDesc.width = _width;
        colorDesc.height = _height;
        colorDesc.format = colorFormat;
        colorDesc.sampleCount = _msaaSamples;
        colorDesc.isRenderTarget = true;
        colorDesc.debugName = "PostProcess::MSAAColor";
        colorDesc.initialState = nvrhi::ResourceStates::RenderTarget;
        colorDesc.keepInitialState = true;
        _msaaColor = _device->createTexture(colorDesc);

        nvrhi::TextureDesc depthDesc;
        depthDesc.width = _width;
        depthDesc.height = _height;
        depthDesc.format = depthFormat;
        depthDesc.sampleCount = _msaaSamples;
        depthDesc.isRenderTarget = true;
        depthDesc.debugName = "PostProcess::MSAADepth";
        depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
        depthDesc.keepInitialState = true;
        _msaaDepth = _device->createTexture(depthDesc);

        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(_msaaColor);
        fbDesc.setDepthAttachment(_msaaDepth);
        _msaaFramebuffer = _device->createFramebuffer(fbDesc);
    }

    if (needsScene)
    {
        nvrhi::TextureDesc colorDesc;
        colorDesc.width = _width;
        colorDesc.height = _height;
        colorDesc.format = colorFormat;
        colorDesc.isRenderTarget = true;
        colorDesc.isShaderResource = true;
        colorDesc.debugName = "PostProcess::SceneColor";
        colorDesc.initialState = nvrhi::ResourceStates::RenderTarget;
        colorDesc.keepInitialState = true;
        _sceneColor = _device->createTexture(colorDesc);

        nvrhi::TextureDesc depthDesc;
        depthDesc.width = _width;
        depthDesc.height = _height;
        depthDesc.format = depthFormat;
        depthDesc.isRenderTarget = true;
        depthDesc.debugName = "PostProcess::SceneDepth";
        depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
        depthDesc.keepInitialState = true;
        _sceneDepth = _device->createTexture(depthDesc);

        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(_sceneColor);
        fbDesc.setDepthAttachment(_sceneDepth);
        _sceneFramebuffer = _device->createFramebuffer(fbDesc);

        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(glm::vec2)));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, _sceneColor));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Sampler(0, _fxaaSampler));
        _fxaaBindingSet = _device->createBindingSet(bindingSetDesc, _fxaaBindingLayout);
    }
}

nvrhi::IFramebuffer *PostProcess::SceneFramebuffer() const
{
    switch (_mode)
    {
    case AaMode::MSAA:      return _msaaFramebuffer;
    case AaMode::FXAA:      return _sceneFramebuffer;
    case AaMode::MSAA_FXAA: return _msaaFramebuffer;
    case AaMode::None:      default: return nullptr;
    }
}

nvrhi::ITexture *PostProcess::SceneColorTexture() const
{
    switch (_mode)
    {
    case AaMode::MSAA:      return _msaaColor;
    case AaMode::FXAA:      return _sceneColor;
    case AaMode::MSAA_FXAA: return _msaaColor;
    case AaMode::None:      default: return nullptr;
    }
}

nvrhi::ITexture *PostProcess::SceneDepthTexture() const
{
    switch (_mode)
    {
    case AaMode::MSAA:      return _msaaDepth;
    case AaMode::FXAA:      return _sceneDepth;
    case AaMode::MSAA_FXAA: return _msaaDepth;
    case AaMode::None:      default: return nullptr;
    }
}

nvrhi::FramebufferInfo PostProcess::SceneFramebufferInfo() const
{
    if (_mode == AaMode::MSAA || _mode == AaMode::MSAA_FXAA)
    {
        nvrhi::FramebufferInfo info = _swapchainInfo;
        info.sampleCount = _msaaSamples;
        return info;
    }
    // None and FXAA both render at sampleCount=1 with the swapchain's own
    // color/depth formats — FXAA's offscreen target is deliberately built to
    // exactly match the swapchain's FramebufferInfo, so scene pipelines never
    // need to be rebuilt for it.
    return _swapchainInfo;
}

void PostProcess::Resolve(nvrhi::ICommandList *commandList, const RenderFrame &frame) const
{
    switch (_mode)
    {
    case AaMode::None:
        return; // scene already rendered directly into the swapchain
    case AaMode::MSAA:
        commandList->resolveTexture(frame.colorTexture, nvrhi::AllSubresources, _msaaColor, nvrhi::AllSubresources);
        return;
    case AaMode::FXAA:
        RunFxaa(commandList, frame);
        return;
    case AaMode::MSAA_FXAA:
        commandList->resolveTexture(_sceneColor, nvrhi::AllSubresources, _msaaColor, nvrhi::AllSubresources);
        RunFxaa(commandList, frame);
        return;
    }
}

void PostProcess::RunFxaa(nvrhi::ICommandList *commandList, const RenderFrame &frame) const
{
    if (!_fxaaPipeline || !_fxaaBindingSet)
    {
        return;
    }

    nvrhi::GraphicsState state;
    state.pipeline = _fxaaPipeline;
    state.framebuffer = frame.framebuffer;
    state.addBindingSet(_fxaaBindingSet);
    state.viewport.addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<float>(frame.width), static_cast<float>(frame.height)));
    commandList->setGraphicsState(state);

    const glm::vec2 texelSize(1.0f / static_cast<float>(frame.width), 1.0f / static_cast<float>(frame.height));
    commandList->setPushConstants(&texelSize, sizeof(texelSize));

    nvrhi::DrawArguments drawArgs;
    drawArgs.vertexCount = 3;
    commandList->draw(drawArgs);
}

} // namespace Assisi::Render
