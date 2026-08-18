/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/PostProcess.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/RenderFrame.hpp>
#include <Assisi/Render/ShaderModule.hpp>
#include <Assisi/Render/Vulkan/VulkanContext.hpp>

namespace Assisi::Render
{
namespace
{
const char *SurfaceName(ChainSurface surface)
{
    switch (surface)
    {
    case ChainSurface::SceneMultisample:   return "SceneMultisample";
    case ChainSurface::SceneHdr:           return "SceneHdr";
    case ChainSurface::OverlayMultisample: return "OverlayMultisample";
    case ChainSurface::Ldr:                return "Ldr";
    case ChainSurface::Swapchain:          break;
    }
    return "Swapchain";
}
} // namespace

void PostProcess::Shutdown()
{
    // Drop every NVRHI handle so the underlying GPU objects are freed now, while
    // the device is still alive. Order-independent — they're ref-counted.
    for (StepResources &step : _stepResources)
    {
        step.bindingSet = nullptr;
        step.pipeline = nullptr;
    }
    _fxaaBindingLayout = nullptr;
    _tonemapBindingLayout = nullptr;
    _sampler = nullptr;
    _fxaaShader = nullptr;
    _tonemapShader = nullptr;
    _fullscreenVertexShader = nullptr;
    _ldrFramebuffer = nullptr;
    _ldrColor = nullptr;
    _overlayMsaaFramebuffer = nullptr;
    _overlayMsaaColor = nullptr;
    _sceneFramebuffer = nullptr;
    _sceneDepth = nullptr;
    _sceneColor = nullptr;
    _msaaFramebuffer = nullptr;
    _msaaDepth = nullptr;
    _msaaColor = nullptr;
    _device = nullptr;
}

bool PostProcess::Initialize(const InitParams &params)
{
    _device = params.device;
    _swapchainInfo = params.swapchainFramebufferInfo;

    _fullscreenVertexShader = LoadSpirvShader(_device, params.vertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    _tonemapShader = LoadSpirvShader(_device, params.tonemapShaderSpvPath, nvrhi::ShaderType::Pixel);
    _fxaaShader = LoadSpirvShader(_device, params.fxaaShaderSpvPath, nvrhi::ShaderType::Pixel);
    if (!_fullscreenVertexShader || !_tonemapShader || !_fxaaShader)
    {
        return false;
    }

    // Only the fragment shaders have bindings (a fullscreen triangle needs no
    // vertex buffer or per-vertex bindings) — see fullscreen.vert. The two
    // layouts differ only in push-constant size, which the shaders disagree on.
    const auto makeLayout = [this](size_t pushConstantSize) {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Pixel;
        desc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, pushConstantSize));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
        desc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
        return _device->createBindingLayout(desc);
    };
    _tonemapBindingLayout = makeLayout(sizeof(TonemapConstants));
    _fxaaBindingLayout = makeLayout(sizeof(glm::vec2));

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true);
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    _sampler = _device->createSampler(samplerDesc);

    return true;
}

void PostProcess::Configure(uint32_t width, uint32_t height, AaMode mode, uint32_t msaaSamples, ChainOptions options)
{
    if (width == 0 || height == 0)
    {
        return; // minimized — keep whatever targets already exist
    }
    if (_width == width && _height == height && _mode == mode && _msaaSamples == msaaSamples &&
        _options.overlays == options.overlays)
    {
        return; // nothing that affects offscreen targets has changed
    }

    _width = width;
    _height = height;
    _mode = mode;
    _msaaSamples = msaaSamples;
    _options = options;
    _plan = PlanChain(mode, options);
    Rebuild();
}

void PostProcess::Rebuild()
{
    for (StepResources &step : _stepResources)
    {
        step.bindingSet = nullptr;
        step.pipeline = nullptr;
    }
    _msaaColor = nullptr;
    _msaaDepth = nullptr;
    _msaaFramebuffer = nullptr;
    _sceneColor = nullptr;
    _sceneDepth = nullptr;
    _sceneFramebuffer = nullptr;
    _overlayMsaaColor = nullptr;
    _overlayMsaaFramebuffer = nullptr;
    _ldrColor = nullptr;
    _ldrFramebuffer = nullptr;

    const nvrhi::Format ldrFormat =
        _swapchainInfo.colorFormats.empty() ? nvrhi::Format::UNKNOWN : _swapchainInfo.colorFormats[0];
    const nvrhi::Format depthFormat = _swapchainInfo.depthFormat;

    // Which surfaces this chain actually touches. Allocating from the plan is
    // what keeps an app that asked for no overlays from paying for the seam.
    const auto uses = [this](ChainSurface surface) {
        if (_plan.sceneTarget == surface)
        {
            return true;
        }
        for (uint32_t i = 0; i < _plan.stepCount; ++i)
        {
            if (_plan.steps[i].source == surface || _plan.steps[i].destination == surface)
            {
                return true;
            }
        }
        return false;
    };

    const auto makeColor = [this](nvrhi::Format format, uint32_t sampleCount, bool shaderResource,
                                  const char *debugName) {
        nvrhi::TextureDesc desc;
        desc.width = _width;
        desc.height = _height;
        desc.format = format;
        desc.sampleCount = sampleCount;
        desc.isRenderTarget = true;
        desc.isShaderResource = shaderResource;
        desc.debugName = debugName;
        desc.initialState = nvrhi::ResourceStates::RenderTarget;
        desc.keepInitialState = true;
        return _device->createTexture(desc);
    };

    const auto makeDepth = [this, depthFormat](uint32_t sampleCount, const char *debugName) {
        nvrhi::TextureDesc desc;
        desc.width = _width;
        desc.height = _height;
        desc.format = depthFormat;
        desc.sampleCount = sampleCount;
        desc.isRenderTarget = true;
        desc.debugName = debugName;
        desc.initialState = nvrhi::ResourceStates::DepthWrite;
        desc.keepInitialState = true;
        return _device->createTexture(desc);
    };

    const auto makeFramebuffer = [this](nvrhi::ITexture *color, nvrhi::ITexture *depth) {
        nvrhi::FramebufferDesc desc;
        desc.addColorAttachment(color);
        if (depth != nullptr)
        {
            desc.setDepthAttachment(depth);
        }
        return _device->createFramebuffer(desc);
    };

    if (uses(ChainSurface::SceneMultisample))
    {
        _msaaColor = makeColor(kSceneColorFormat, _msaaSamples, /*shaderResource=*/false, "PostProcess::MSAAColor");
        _msaaDepth = makeDepth(_msaaSamples, "PostProcess::MSAADepth");
        _msaaFramebuffer = makeFramebuffer(_msaaColor, _msaaDepth);
    }

    // Always present: the tone map reads it in every configuration. It only needs
    // its own depth when the scene draws into it, which is when MSAA is off.
    const bool sceneDrawsHere = (_plan.sceneTarget == ChainSurface::SceneHdr);
    _sceneColor = makeColor(kSceneColorFormat, 1, /*shaderResource=*/true, "PostProcess::SceneColor");
    if (sceneDrawsHere)
    {
        _sceneDepth = makeDepth(1, "PostProcess::SceneDepth");
    }
    _sceneFramebuffer = makeFramebuffer(_sceneColor, _sceneDepth);

    if (uses(ChainSurface::OverlayMultisample))
    {
        // Shares the scene's depth buffer rather than owning one: overlays must
        // test against what the scene wrote, and a depth buffer cannot be copied
        // between sample counts.
        _overlayMsaaColor =
            makeColor(ldrFormat, _msaaSamples, /*shaderResource=*/false, "PostProcess::OverlayMSAAColor");
        _overlayMsaaFramebuffer = makeFramebuffer(_overlayMsaaColor, _msaaDepth);
    }

    if (uses(ChainSurface::Ldr))
    {
        _ldrColor = makeColor(ldrFormat, 1, /*shaderResource=*/true, "PostProcess::LdrColor");
        // Depth only where overlays draw straight into it (no MSAA), for the same
        // reason the multisample overlay target has one.
        const bool overlaysDrawHere = _plan.OverlaySurface() == ChainSurface::Ldr;
        _ldrFramebuffer = makeFramebuffer(_ldrColor, overlaysDrawHere ? _sceneDepth.Get() : nullptr);
    }

    // One pipeline and binding set per fullscreen step: each is built against the
    // framebuffer it draws into and bound to the texture it reads.
    for (uint32_t i = 0; i < _plan.stepCount; ++i)
    {
        const ChainStep &step = _plan.steps[i];
        if (step.stage == ChainStage::Resolve || step.stage == ChainStage::Overlays)
        {
            continue; // no pipeline of ours
        }

        const bool isFxaa = (step.stage == ChainStage::Fxaa);
        nvrhi::IBindingLayout *layout = isFxaa ? _fxaaBindingLayout.Get() : _tonemapBindingLayout.Get();

        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
        pipelineDesc.VS = _fullscreenVertexShader;
        pipelineDesc.PS = isFxaa ? _fxaaShader : _tonemapShader;
        pipelineDesc.addBindingLayout(layout);
        pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        // The overlay target carries the scene's depth buffer, which these passes
        // must leave exactly as the scene wrote it — the overlays still need it.
        pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
        pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;

        _stepResources[i].pipeline = _device->createGraphicsPipeline(pipelineDesc, InfoFor(step.destination));
        if (_stepResources[i].pipeline == nullptr)
        {
            Core::Log::Error("PostProcess: failed to create the pipeline for the pass writing {}.",
                             SurfaceName(step.destination));
            continue;
        }

        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(
            0, isFxaa ? sizeof(glm::vec2) : sizeof(TonemapConstants)));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, TextureFor(step.source, nullptr)));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Sampler(0, _sampler));
        _stepResources[i].bindingSet = _device->createBindingSet(bindingSetDesc, layout);
    }
}

nvrhi::FramebufferInfo PostProcess::InfoFor(ChainSurface surface) const
{
    if (surface == ChainSurface::Swapchain)
    {
        return _swapchainInfo;
    }

    nvrhi::FramebufferInfo info = _swapchainInfo;
    if (IsHdrSurface(surface))
    {
        info.colorFormats.resize(1);
        info.colorFormats[0] = kSceneColorFormat;
    }
    info.sampleCount = IsMultisampleSurface(surface) ? _msaaSamples : 1;
    // Only the targets the scene or the overlays draw into carry depth; the tone
    // map's own output does not when nothing after it depth-tests.
    const bool hasDepth = (surface == _plan.sceneTarget) || (surface == _plan.OverlaySurface());
    info.depthFormat = hasDepth ? _swapchainInfo.depthFormat : nvrhi::Format::UNKNOWN;
    return info;
}

nvrhi::IFramebuffer *PostProcess::SceneFramebuffer() const
{
    return FramebufferFor(_plan.sceneTarget, nullptr);
}

nvrhi::ITexture *PostProcess::SceneColorTexture() const
{
    return _plan.sceneTarget == ChainSurface::SceneMultisample ? _msaaColor.Get() : _sceneColor.Get();
}

nvrhi::ITexture *PostProcess::SceneDepthTexture() const
{
    return _plan.sceneTarget == ChainSurface::SceneMultisample ? _msaaDepth.Get() : _sceneDepth.Get();
}

nvrhi::FramebufferInfo PostProcess::SceneFramebufferInfo() const
{
    return InfoFor(_plan.sceneTarget);
}

nvrhi::IFramebuffer *PostProcess::OverlayFramebuffer() const
{
    if (!_plan.Has(ChainStage::Overlays))
    {
        return nullptr;
    }
    return _plan.OverlaySurface() == ChainSurface::OverlayMultisample ? _overlayMsaaFramebuffer.Get()
                                                                      : _ldrFramebuffer.Get();
}

nvrhi::FramebufferInfo PostProcess::OverlayFramebufferInfo() const
{
    return InfoFor(_plan.OverlaySurface());
}

// `frame` may be null when the caller is asking about an offscreen surface —
// the swapchain is the one surface this class does not own.
nvrhi::IFramebuffer *PostProcess::FramebufferFor(ChainSurface surface, const RenderFrame *frame) const
{
    switch (surface)
    {
    case ChainSurface::SceneMultisample:   return _msaaFramebuffer;
    case ChainSurface::SceneHdr:           return _sceneFramebuffer;
    case ChainSurface::OverlayMultisample: return _overlayMsaaFramebuffer;
    case ChainSurface::Ldr:                return _ldrFramebuffer;
    case ChainSurface::Swapchain:          break;
    }
    return frame != nullptr ? frame->framebuffer : nullptr;
}

nvrhi::ITexture *PostProcess::TextureFor(ChainSurface surface, const RenderFrame *frame) const
{
    switch (surface)
    {
    case ChainSurface::SceneMultisample:   return _msaaColor;
    case ChainSurface::SceneHdr:           return _sceneColor;
    case ChainSurface::OverlayMultisample: return _overlayMsaaColor;
    case ChainSurface::Ldr:                return _ldrColor;
    case ChainSurface::Swapchain:          break;
    }
    return frame != nullptr ? frame->colorTexture : nullptr;
}

void PostProcess::RunBeforeOverlays(nvrhi::ICommandList *commandList, const RenderFrame &frame) const
{
    RunSteps(commandList, frame, 0, _plan.IndexOf(ChainStage::Overlays));
}

void PostProcess::RunAfterOverlays(nvrhi::ICommandList *commandList, const RenderFrame &frame) const
{
    const uint32_t overlays = _plan.IndexOf(ChainStage::Overlays);
    // IndexOf returns stepCount when there is no overlay seam, which makes this
    // an empty range — RunBeforeOverlays already ran the whole chain.
    RunSteps(commandList, frame, overlays < _plan.stepCount ? overlays + 1 : _plan.stepCount, _plan.stepCount);
}

void PostProcess::RunSteps(nvrhi::ICommandList *commandList, const RenderFrame &frame, uint32_t first,
                           uint32_t last) const
{
    for (uint32_t i = first; i < last; ++i)
    {
        const ChainStep &step = _plan.steps[i];
        if (step.stage == ChainStage::Overlays)
        {
            continue; // the app's, not ours
        }
        if (step.stage == ChainStage::Resolve)
        {
            commandList->resolveTexture(TextureFor(step.destination, &frame), nvrhi::AllSubresources,
                                        TextureFor(step.source, &frame), nvrhi::AllSubresources);
            continue;
        }

        const StepResources &resources = _stepResources[i];
        if (!resources.pipeline || !resources.bindingSet)
        {
            continue;
        }

        nvrhi::GraphicsState state;
        state.pipeline = resources.pipeline;
        state.framebuffer = FramebufferFor(step.destination, &frame);
        state.addBindingSet(resources.bindingSet);
        state.viewport.addViewportAndScissorRect(
            nvrhi::Viewport(static_cast<float>(frame.width), static_cast<float>(frame.height)));
        commandList->setGraphicsState(state);

        if (step.stage == ChainStage::Fxaa)
        {
            const glm::vec2 texelSize(1.0f / static_cast<float>(frame.width), 1.0f / static_cast<float>(frame.height));
            commandList->setPushConstants(&texelSize, sizeof(texelSize));
        }
        else
        {
            // A Blit is the tone map shader told to copy: by the time it runs, the
            // image it is moving has already been mapped, exposed and graded.
            const bool copyThrough = (step.stage == ChainStage::Blit) || _tonemapPassthrough;
            const TonemapConstants constants = MakeTonemapConstants(_tonemap, copyThrough);
            commandList->setPushConstants(&constants, sizeof(constants));
        }

        nvrhi::DrawArguments drawArgs;
        drawArgs.vertexCount = 3;
        commandList->draw(drawArgs);
    }
}

} // namespace Assisi::Render
