/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ShadowPass.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/GpuMarker.hpp>

#include <algorithm>

namespace Assisi::Render
{
namespace
{
nvrhi::Format DepthFormat(ShadowMapFormat format)
{
    return format == ShadowMapFormat::D16 ? nvrhi::Format::D16 : nvrhi::Format::D32;
}
} // namespace

bool ShadowPass::Initialize(const InitParams &params)
{
    _device = params.device;
    _depthRenderer = params.depthRenderer;
    if (_device == nullptr || _depthRenderer == nullptr || !_depthRenderer->IsReady())
    {
        return false;
    }
    return CreatePlaceholder();
}

bool ShadowPass::CreatePlaceholder()
{
    nvrhi::TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.arraySize = 1;
    desc.dimension = nvrhi::TextureDimension::Texture2DArray;
    desc.format = nvrhi::Format::D32;
    desc.isShaderResource = true;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName = "ShadowPass::NoCascades";
    _placeholderTexture = _device->createTexture(desc);
    if (_placeholderTexture == nullptr)
    {
        Core::Log::Error("ShadowPass: failed to create the placeholder cascade texture.");
        return false;
    }
    _cascadeTexture = _placeholderTexture;
    return true;
}

void ShadowPass::ReleaseTargets()
{
    _cascadeFramebuffers.clear();
    _cascadeTexture = _placeholderTexture;
    _builtCascades = 0;
    _builtResolution = 0;
}

bool ShadowPass::RebuildTargets()
{
    _cascadeFramebuffers.clear();

    nvrhi::TextureDesc desc;
    desc.width = _settings.resolution;
    desc.height = _settings.resolution;
    desc.arraySize = _settings.cascadeCount;
    desc.dimension = nvrhi::TextureDimension::Texture2DArray;
    desc.format = DepthFormat(_settings.format);
    desc.isRenderTarget = true;
    desc.isShaderResource = true;
    // Written as depth every frame and read as an SRV in the same frame; nvrhi's
    // automatic barriers move it between the two and restore this on the way out.
    desc.initialState = nvrhi::ResourceStates::DepthWrite;
    desc.keepInitialState = true;
    desc.debugName = "ShadowPass::Cascades";

    nvrhi::TextureHandle texture = _device->createTexture(desc);
    if (texture == nullptr)
    {
        Core::Log::Error("ShadowPass: failed to allocate the {}x{} x{} cascade array.", _settings.resolution,
                         _settings.resolution, _settings.cascadeCount);
        return false;
    }

    // One framebuffer per slice rather than one layered framebuffer: each
    // cascade is drawn with its own matrix, so there is nothing for a
    // layered pass to amortise without a geometry stage this does not have.
    _cascadeFramebuffers.reserve(_settings.cascadeCount);
    for (std::uint32_t i = 0; i < _settings.cascadeCount; ++i)
    {
        nvrhi::FramebufferDesc framebufferDesc;
        framebufferDesc.setDepthAttachment(
            nvrhi::FramebufferAttachment().setTexture(texture).setArraySlice(static_cast<nvrhi::ArraySlice>(i)));
        nvrhi::FramebufferHandle framebuffer = _device->createFramebuffer(framebufferDesc);
        if (framebuffer == nullptr)
        {
            Core::Log::Error("ShadowPass: failed to create the framebuffer for cascade {}.", i);
            _cascadeFramebuffers.clear();
            return false;
        }
        _cascadeFramebuffers.push_back(std::move(framebuffer));
    }

    _cascadeTexture = std::move(texture);
    _builtCascades = _settings.cascadeCount;
    _builtResolution = _settings.resolution;
    _builtFormat = _settings.format;
    return true;
}

bool ShadowPass::RebuildPipeline()
{
    if (_cascadeFramebuffers.empty())
    {
        return false;
    }

    _pipeline = _depthRenderer->CreatePipeline(_cascadeFramebuffers.front(), _settings.slopeBias);
    if (_pipeline == nullptr)
    {
        return false;
    }
    _builtSlopeBias = _settings.slopeBias;
    return true;
}

bool ShadowPass::Configure(const SunShadowSettings &settings, bool active)
{
    if (_device == nullptr || _depthRenderer == nullptr || !_depthRenderer->IsReady())
    {
        return false; // Initialize failed; the pass stays inactive for good
    }

    if (!active)
    {
        if (_active)
        {
            // Nothing wants shadows any more: give the memory back rather than
            // holding a 4-cascade array against a scene with no sun in it.
            ReleaseTargets();
            _pipeline = nullptr;
            _active = false;
        }
        return true;
    }

    const SunShadowSettings safe = Sanitized(settings);
    const bool targetsStale = _cascadeFramebuffers.empty() || safe.cascadeCount != _builtCascades ||
                              safe.resolution != _builtResolution || safe.format != _builtFormat;
    const bool pipelineStale = _pipeline == nullptr || safe.slopeBias != _builtSlopeBias;
    _settings = safe;

    if (!targetsStale && !pipelineStale)
    {
        _active = true;
        return true;
    }

    if (targetsStale && !RebuildTargets())
    {
        ReleaseTargets();
        _pipeline = nullptr;
        _active = false;
        return false;
    }
    // A new array means new framebuffers, and a pipeline is built against one.
    if ((targetsStale || pipelineStale) && !RebuildPipeline())
    {
        ReleaseTargets();
        _pipeline = nullptr;
        _active = false;
        return false;
    }

    _active = true;
    return true;
}

ShadowPass::Stats ShadowPass::Render(nvrhi::ICommandList *commandList, const CascadeFit &fit,
                                     std::span<const ShadowCaster> casters) const
{
    Stats stats;
    if (!IsActive() || commandList == nullptr || fit.count == 0)
    {
        return stats;
    }

    const std::uint32_t cascadeCount = std::min<std::uint32_t>(fit.count, _builtCascades);

    ASSISI_PROFILE_GPU_PASS(commandList, "shadow-cascades");

    _scratchTargets.clear();
    _scratchTargets.reserve(cascadeCount);
    for (std::uint32_t cascade = 0; cascade < cascadeCount; ++cascade)
    {
        // Cleared whether or not anything draws into it: a stale slice would
        // shadow this frame with last frame's geometry. The clear is this
        // pass's business rather than the renderer's, because a cascade is
        // rebuilt every frame and a cached map would be ruined by one.
        commandList->clearDepthStencilTexture(
            _cascadeTexture, nvrhi::TextureSubresourceSet(0, 1, static_cast<nvrhi::ArraySlice>(cascade), 1), true, 1.0f,
            false, 0);

        _scratchTargets.push_back(
            ShadowDepthTarget{.view = CascadeShadowView(fit.cascades[cascade], cascade, _settings),
                              .framebuffer = _cascadeFramebuffers[cascade]});
    }

    const ShadowDepthRenderer::Stats drawn = _depthRenderer->Render(commandList, _pipeline, _scratchTargets, casters);

    _firstView = drawn.firstView;
    stats.cascades = drawn.views;
    stats.instances = drawn.instances;
    stats.batches = drawn.batches;
    stats.drawCalls = drawn.drawCalls;
    stats.culled = drawn.culled;
    return stats;
}

} // namespace Assisi::Render
