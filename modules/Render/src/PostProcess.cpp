/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/PostProcess.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Exposure.hpp>
#include <Assisi/Render/RenderFrame.hpp>
#include <Assisi/Render/ShaderModule.hpp>

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
    _tonemap = FullscreenDraw{};
    _fxaa = FullscreenDraw{};
    _copy = FullscreenDraw{};
    _noExposure = nullptr;
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
    // vertex buffer or per-vertex bindings) — see fullscreen.vert.
    {
        nvrhi::BindingLayoutDesc tonemap;
        tonemap.visibility = nvrhi::ShaderType::Pixel;
        tonemap.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(TonemapConstants)));
        tonemap.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
        // The bloom's glow, and the automatic exposure.
        tonemap.addItem(nvrhi::BindingLayoutItem::Texture_SRV(1));
        tonemap.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2));
        tonemap.addItem(nvrhi::BindingLayoutItem::Sampler(0));
        _tonemapBindingLayout = _device->createBindingLayout(tonemap);

        nvrhi::BindingLayoutDesc fxaa;
        fxaa.visibility = nvrhi::ShaderType::Pixel;
        fxaa.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(glm::vec2)));
        fxaa.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
        fxaa.addItem(nvrhi::BindingLayoutItem::Sampler(0));
        _fxaaBindingLayout = _device->createBindingLayout(fxaa);
    }

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true);
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    _sampler = _device->createSampler(samplerDesc);

    nvrhi::BufferDesc noExposure;
    noExposure.byteSize = sizeof(ExposureState);
    noExposure.structStride = sizeof(ExposureState);
    noExposure.initialState = nvrhi::ResourceStates::ShaderResource;
    noExposure.keepInitialState = true;
    noExposure.debugName = "PostProcess::NoExposure";
    _noExposure = _device->createBuffer(noExposure);
    return _noExposure != nullptr;
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
    _route = RouteChain(mode, options);
    Rebuild();
}

void PostProcess::Rebuild()
{
    _tonemap = FullscreenDraw{};
    _fxaa = FullscreenDraw{};
    _copy = FullscreenDraw{};
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
                               // The scene distance pass reads it when SSAO is on.
                               desc.isShaderResource = true;
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

    // Only the surfaces this route touches are allocated, which is what keeps
    // an app that asked for no overlays from paying for the seam.
    if (_route.sceneTarget == ChainSurface::SceneMultisample)
    {
        _msaaColor = makeColor(kSceneColorFormat, _msaaSamples, /*shaderResource=*/ false, "PostProcess::MSAAColor");
        _msaaDepth = makeDepth(_msaaSamples, "PostProcess::MSAADepth");
        _msaaFramebuffer = makeFramebuffer(_msaaColor, _msaaDepth);
    }

    // Always present: the tone map reads it in every configuration. It only needs
    // its own depth when the scene draws into it, which is when MSAA is off.
    _sceneColor = makeColor(kSceneColorFormat, 1, /*shaderResource=*/ true, "PostProcess::SceneColor");
    if (_route.sceneTarget == ChainSurface::SceneHdr)
    {
        _sceneDepth = makeDepth(1, "PostProcess::SceneDepth");
    }
    _sceneFramebuffer = makeFramebuffer(_sceneColor, _sceneDepth);

    if (_route.toneMapped == ChainSurface::OverlayMultisample)
    {
        // Shares the scene's depth buffer rather than owning one: overlays must
        // test against what the scene wrote, and a depth buffer cannot be copied
        // between sample counts.
        _overlayMsaaColor =
            makeColor(ldrFormat, _msaaSamples, /*shaderResource=*/ false, "PostProcess::OverlayMSAAColor");
        _overlayMsaaFramebuffer = makeFramebuffer(_overlayMsaaColor, _msaaDepth);
    }

    if (_route.toneMapped == ChainSurface::Ldr || _route.display == ChainSurface::Ldr)
    {
        _ldrColor = makeColor(ldrFormat, 1, /*shaderResource=*/ true, "PostProcess::LdrColor");
        // Depth only where overlays draw straight into it (no MSAA), for the same
        // reason the multisample overlay target has one.
        _ldrFramebuffer = makeFramebuffer(_ldrColor, CarriesSceneDepth(ChainSurface::Ldr) ? _sceneDepth.Get() : nullptr);
    }

    // Each draw is built against the surface it draws into; its bindings follow
    // on its first draw, from the textures it is handed.
    _tonemap.pipeline = MakePipeline(_tonemapShader, _tonemapBindingLayout, _route.toneMapped);
    if (_route.fxaa)
    {
        _fxaa.pipeline = MakePipeline(_fxaaShader, _fxaaBindingLayout, ChainSurface::Swapchain);
    }
    if (_route.CopiesToSwapchain())
    {
        _copy.pipeline = MakePipeline(_tonemapShader, _tonemapBindingLayout, ChainSurface::Swapchain);
    }
}

nvrhi::GraphicsPipelineHandle PostProcess::MakePipeline(nvrhi::IShader *shader, nvrhi::IBindingLayout *layout,
                                                        ChainSurface destination) const
{
    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.VS = _fullscreenVertexShader;
    pipelineDesc.PS = shader;
    pipelineDesc.addBindingLayout(layout);
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    // The overlay target carries the scene's depth buffer, which these passes
    // must leave exactly as the scene wrote it — the overlays still need it.
    pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;

    nvrhi::GraphicsPipelineHandle pipeline = _device->createGraphicsPipeline(pipelineDesc, InfoFor(destination));
    if (pipeline == nullptr)
    {
        Core::Log::Error("PostProcess: failed to create the pipeline for the pass writing {}.",
                         SurfaceName(destination));
    }
    return pipeline;
}

void PostProcess::BindTonemapInputs(FullscreenDraw &draw, nvrhi::ITexture *source, nvrhi::ITexture *glow,
                                    nvrhi::IBuffer *exposure) const
{
    if (draw.bindings != nullptr && draw.source == source && draw.glow == glow && draw.exposure == exposure)
    {
        return;
    }
    draw.source = source;
    draw.glow = glow;
    draw.exposure = exposure;

    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(TonemapConstants)));
    desc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, source));
    // Without a glow the source fills the slot; the shader reads it only while
    // it mixes glow in.
    desc.addItem(nvrhi::BindingSetItem::Texture_SRV(1, glow != nullptr ? glow : source));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, exposure != nullptr ? exposure : _noExposure.Get()));
    desc.addItem(nvrhi::BindingSetItem::Sampler(0, _sampler));
    draw.bindings = _device->createBindingSet(desc, _tonemapBindingLayout);
}

void PostProcess::Draw(const RenderFrame &frame, const FullscreenDraw &draw, const void *constants,
                       uint32_t constantsSize)
{
    if (!draw.pipeline || !draw.bindings)
    {
        return;
    }
    {
        // Where a full-screen step's CPU cost actually is: nvrhi resolves the
        // binding set and commits the barriers the target needs here, while the
        // draw below is three vertices and a command.
        ASSISI_PROFILE_SCOPE("graphics-state");
        nvrhi::GraphicsState state;
        state.pipeline = draw.pipeline;
        state.framebuffer = frame.framebuffer;
        state.addBindingSet(draw.bindings);
        state.viewport.addViewportAndScissorRect(
            nvrhi::Viewport(static_cast<float>(frame.width), static_cast<float>(frame.height)));
        frame.commandList->setGraphicsState(state);
    }
    {
        ASSISI_PROFILE_SCOPE("push-constants");
        frame.commandList->setPushConstants(constants, constantsSize);
    }
    {
        ASSISI_PROFILE_SCOPE("draw");
        nvrhi::DrawArguments drawArgs;
        drawArgs.vertexCount = 3;
        frame.commandList->draw(drawArgs);
    }
}

void PostProcess::Tonemap(const RenderFrame &frame, nvrhi::ITexture *scene, nvrhi::ITexture *glow,
                          nvrhi::IBuffer *exposure, const TonemapConstants &constants)
{
    BindTonemapInputs(_tonemap, scene, glow, exposure);
    Draw(frame, _tonemap, &constants, sizeof(constants));
}

void PostProcess::Fxaa(const RenderFrame &frame, nvrhi::ITexture *display)
{
    if (_fxaa.bindings == nullptr || _fxaa.source != display)
    {
        _fxaa.source = display;
        nvrhi::BindingSetDesc desc;
        desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(glm::vec2)));
        desc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, display));
        desc.addItem(nvrhi::BindingSetItem::Sampler(0, _sampler));
        _fxaa.bindings = _device->createBindingSet(desc, _fxaaBindingLayout);
    }
    const glm::vec2 texelSize(1.0f / static_cast<float>(frame.width), 1.0f / static_cast<float>(frame.height));
    Draw(frame, _fxaa, &texelSize, sizeof(texelSize));
}

void PostProcess::Copy(const RenderFrame &frame, nvrhi::ITexture *display)
{
    BindTonemapInputs(_copy, display, nullptr, nullptr);
    // The shader returns its input before it reads anything else, so the rest
    // of the constants are never looked at.
    TonemapConstants constants;
    constants.passthrough = 1u;
    Draw(frame, _copy, &constants, sizeof(constants));
}

bool PostProcess::CarriesSceneDepth(ChainSurface surface) const
{
    return surface == _route.sceneTarget || (_route.overlays && surface == _route.toneMapped);
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
    info.depthFormat = CarriesSceneDepth(surface) ? _swapchainInfo.depthFormat : nvrhi::Format::UNKNOWN;
    return info;
}

RenderTarget PostProcess::Target(ChainSurface surface, const RenderTarget &swapchain) const
{
    if (surface == ChainSurface::Swapchain)
    {
        return swapchain;
    }
    return RenderTarget{.framebuffer = FramebufferFor(surface),
                        .colorTexture = TextureFor(surface),
                        .depthTexture = CarriesSceneDepth(surface) ? SceneDepthTexture() : nullptr,
                        .width = _width,
                        .height = _height};
}

nvrhi::IFramebuffer *PostProcess::SceneFramebuffer() const
{
    return FramebufferFor(_route.sceneTarget);
}

nvrhi::ITexture *PostProcess::SceneColorTexture() const
{
    return TextureFor(_route.sceneTarget);
}

nvrhi::ITexture *PostProcess::SceneDepthTexture() const
{
    return _route.sceneTarget == ChainSurface::SceneMultisample ? _msaaDepth.Get() : _sceneDepth.Get();
}

nvrhi::FramebufferInfo PostProcess::SceneFramebufferInfo() const
{
    return InfoFor(_route.sceneTarget);
}

nvrhi::IFramebuffer *PostProcess::OverlayFramebuffer() const
{
    return _route.overlays ? FramebufferFor(_route.toneMapped) : nullptr;
}

nvrhi::FramebufferInfo PostProcess::OverlayFramebufferInfo() const
{
    return InfoFor(_route.overlays ? _route.toneMapped : ChainSurface::Swapchain);
}

nvrhi::IFramebuffer *PostProcess::FramebufferFor(ChainSurface surface) const
{
    switch (surface)
    {
    case ChainSurface::SceneMultisample:   return _msaaFramebuffer;
    case ChainSurface::SceneHdr:           return _sceneFramebuffer;
    case ChainSurface::OverlayMultisample: return _overlayMsaaFramebuffer;
    case ChainSurface::Ldr:                return _ldrFramebuffer;
    case ChainSurface::Swapchain:          break;
    }
    return nullptr;
}

nvrhi::ITexture *PostProcess::TextureFor(ChainSurface surface) const
{
    switch (surface)
    {
    case ChainSurface::SceneMultisample:   return _msaaColor;
    case ChainSurface::SceneHdr:           return _sceneColor;
    case ChainSurface::OverlayMultisample: return _overlayMsaaColor;
    case ChainSurface::Ldr:                return _ldrColor;
    case ChainSurface::Swapchain:          break;
    }
    return nullptr;
}

} // namespace Assisi::Render
