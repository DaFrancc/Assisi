/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/LocalShadowPass.hpp>

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

void LocalShadowCasterIndex::Clear()
{
    start.clear();
    caster.clear();
}

bool LocalShadowPass::Initialize(const InitParams &params)
{
    _device = params.device;
    _depthRenderer = params.depthRenderer;
    if (_device == nullptr || _depthRenderer == nullptr || !_depthRenderer->IsReady())
    {
        return false;
    }
    return CreateNoAtlasTexture();
}

bool LocalShadowPass::CreateNoAtlasTexture()
{
    nvrhi::TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.arraySize = 1;
    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.format = nvrhi::Format::D32;
    desc.isShaderResource = true;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName = "LocalShadowPass::NoAtlas";
    _noAtlasTexture = _device->createTexture(desc);
    if (_noAtlasTexture == nullptr)
    {
        Core::Log::Error("LocalShadowPass: failed to create the empty atlas texture.");
        return false;
    }
    _atlasTexture = _noAtlasTexture;
    return true;
}

void LocalShadowPass::ReleaseTargets()
{
    _atlasFramebuffer = nullptr;
    _atlasTexture = _noAtlasTexture;
    _builtResolution = 0;
    _tiles.clear();
    _targets.clear();
}

bool LocalShadowPass::RebuildTargets()
{
    _atlasFramebuffer = nullptr;

    nvrhi::TextureDesc desc;
    desc.width = _settings.atlasResolution;
    desc.height = _settings.atlasResolution;
    desc.arraySize = 1;
    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.format = DepthFormat(_settings.format);
    desc.isRenderTarget = true;
    desc.isShaderResource = true;
    desc.initialState = nvrhi::ResourceStates::DepthWrite;
    desc.keepInitialState = true;
    desc.debugName = "LocalShadowPass::Atlas";

    nvrhi::TextureHandle texture = _device->createTexture(desc);
    if (texture == nullptr)
    {
        Core::Log::Error("LocalShadowPass: failed to allocate the {}x{} shadow atlas.", _settings.atlasResolution,
                         _settings.atlasResolution);
        return false;
    }

    nvrhi::FramebufferDesc framebufferDesc;
    framebufferDesc.setDepthAttachment(nvrhi::FramebufferAttachment().setTexture(texture));
    nvrhi::FramebufferHandle framebuffer = _device->createFramebuffer(framebufferDesc);
    if (framebuffer == nullptr)
    {
        Core::Log::Error("LocalShadowPass: failed to create the atlas framebuffer.");
        return false;
    }

    _atlasTexture = std::move(texture);
    _atlasFramebuffer = std::move(framebuffer);
    _builtResolution = _settings.atlasResolution;
    _builtFormat = _settings.format;
    return true;
}

bool LocalShadowPass::RebuildPipelines()
{
    if (_atlasFramebuffer == nullptr)
    {
        return false;
    }

    // Sized against the smallest tile the atlas can hand out, which is the
    // coarsest map any light here will be sampled from. A cap sized for the
    // largest tile would be too tight for a demoted light and let acne through
    // on exactly the lights that were already the worst served.
    //
    // One value for every tile because the cap is rasterizer state, and that is
    // baked into the pipeline rather than set per draw. Serving each size class
    // its own cap means a pipeline set per class, which is the honest version of
    // "per-tile bias auto-scaled by tile resolution" and is not what this does.
    const float slopeBiasClamp = LocalSlopeBiasClampNdc(kMinShadowFaceResolution);

    for (std::uint32_t index = 0; index < kMeshPipelineCount; ++index)
    {
        _pipelines[index] = _depthRenderer->CreatePipeline(_atlasFramebuffer, static_cast<MeshPipeline>(index),
                                                           _settings.slopeBias, slopeBiasClamp);
    }
    if (_pipelines[static_cast<std::uint32_t>(MeshPipeline::Opaque)] == nullptr)
    {
        return false;
    }
    _builtSlopeBias = _settings.slopeBias;
    return true;
}

bool LocalShadowPass::Configure(const LocalShadowSettings &settings, bool active)
{
    if (_device == nullptr || _depthRenderer == nullptr || !_depthRenderer->IsReady())
    {
        return false; // Initialize failed; the pass stays inactive for good
    }

    if (!active)
    {
        if (_active)
        {
            ReleaseTargets();
            _pipelines = {};
            _active = false;
        }
        return true;
    }

    const LocalShadowSettings safe = Sanitized(settings);
    const bool targetsStale = _atlasFramebuffer == nullptr || safe.atlasResolution != _builtResolution ||
                              safe.format != _builtFormat;
    const bool pipelinesStale = _pipelines[static_cast<std::uint32_t>(MeshPipeline::Opaque)] == nullptr ||
                                safe.slopeBias != _builtSlopeBias;
    _settings = safe;

    if (!targetsStale && !pipelinesStale)
    {
        _active = true;
        return true;
    }

    if (targetsStale && !RebuildTargets())
    {
        ReleaseTargets();
        _pipelines = {};
        _active = false;
        return false;
    }
    if ((targetsStale || pipelinesStale) && !RebuildPipelines())
    {
        ReleaseTargets();
        _pipelines = {};
        _active = false;
        return false;
    }

    _active = true;
    return true;
}

ShadowPipelines LocalShadowPass::PipelineSet() const
{
    ShadowPipelines set;
    for (std::uint32_t index = 0; index < kMeshPipelineCount; ++index)
    {
        set.byPipeline[index] = _pipelines[index];
    }
    return set;
}

std::uint32_t LocalShadowPass::AllocateTiles(std::span<const LocalShadowRequest> requests)
{
    _tiles.clear();
    _targets.clear();
    _tileViewStart.clear();
    _tileViewStart.push_back(0);

    _allocator.Reset(_settings.atlasResolution);

    std::uint32_t unserved = 0;
    for (const LocalShadowRequest &request : requests)
    {
        const std::uint32_t faces = LocalShadowFaceCount(request.kind);

        // Demote until the atlas can serve every one of this light's faces at
        // one class. Asked before anything is cut, because a tile handed out
        // cannot be given back until the next Reset — so a point light that ran
        // out on its sixth face would have stranded five.
        std::uint32_t sizeClass = std::min(request.sizeClass, kShadowSizeClassCount - 1u);
        while (_allocator.Available(sizeClass) < faces && sizeClass > 0u)
        {
            --sizeClass;
        }
        if (_allocator.Available(sizeClass) < faces)
        {
            ++unserved;
            continue;
        }

        const auto firstView = static_cast<std::uint32_t>(_targets.size());
        std::uint32_t resolution = 0;
        for (std::uint32_t face = 0; face < faces; ++face)
        {
            const ShadowViewRect rect = _allocator.Allocate(sizeClass);
            resolution = rect.width;
            const ShadowView view =
                request.kind == LocalLightKind::Point
                ? PointFaceShadowView(request.position, request.range, face, rect, _settings.atlasResolution,
                                      _settings)
                : SpotShadowView(request.position, request.direction, request.range, request.outerAngleDegrees, rect,
                                 _settings.atlasResolution, _settings);
            _targets.push_back(ShadowDepthTarget{.view = view, .framebuffer = _atlasFramebuffer});
        }

        _tiles.push_back(Tile{.kind = request.kind,
                              .lightIndex = request.lightIndex,
                              .firstView = firstView,
                              .resolution = resolution});
        _tileViewStart.push_back(static_cast<std::uint32_t>(_targets.size()));
    }
    return unserved;
}

ShadowDepthRenderer::Stats LocalShadowPass::RenderChunk(nvrhi::ICommandList *commandList, std::uint32_t firstTile,
                                                        std::uint32_t tileCount, std::uint32_t firstView,
                                                        std::uint32_t viewCount,
                                                        std::span<const ShadowCaster> casters,
                                                        const LocalShadowCasterIndex &casterIndex)
{
    // Stamped rather than cleared: a chunk touches the casters its lights reach,
    // and clearing a span sized to the whole scene for each chunk would cost
    // more than the chunk does.
    ++_chunkStamp;
    _casterMask.resize(casters.size());
    _casterStamp.resize(casters.size(), 0);

    for (std::uint32_t tile = firstTile; tile < firstTile + tileCount; ++tile)
    {
        // Every face of this light takes the same bit set, because the light was
        // culled once and the faces refine it. The bits are chunk-local, which is
        // what keeps a mask inside kShadowViewMaskBits however many views the
        // frame draws in total.
        const std::uint32_t viewBase = _tileViewStart[tile] - firstView;
        const std::uint32_t faces = _tileViewStart[tile + 1u] - _tileViewStart[tile];
        std::uint32_t bits = 0;
        for (std::uint32_t face = 0; face < faces; ++face)
        {
            bits |= 1u << (viewBase + face);
        }

        const std::uint32_t rowEnd = casterIndex.start[tile + 1u];
        for (std::uint32_t entry = casterIndex.start[tile]; entry < rowEnd; ++entry)
        {
            const std::uint32_t index = casterIndex.caster[entry];
            if (index >= casters.size())
            {
                continue;
            }
            if (_casterStamp[index] != _chunkStamp)
            {
                _casterStamp[index] = _chunkStamp;
                _casterMask[index] = 0;
            }
            _casterMask[index] |= bits;
        }
    }

    // One sweep in the caster span's own order, so the chunk's list stays sorted
    // by pipeline class and geometry key exactly as the span is — which is what
    // lets consecutive entries still coalesce into one instanced draw.
    _chunkCasters.clear();
    for (std::uint32_t index = 0; index < casters.size(); ++index)
    {
        if (_casterStamp[index] != _chunkStamp || _casterMask[index] == 0u)
        {
            continue;
        }
        ShadowCaster caster = casters[index];
        caster.viewMask = _casterMask[index];
        _chunkCasters.push_back(caster);
    }

    return _depthRenderer->Render(commandList, PipelineSet(),
                                  std::span<const ShadowDepthTarget>(_targets).subspan(firstView, viewCount),
                                  _chunkCasters);
}

LocalShadowPass::Stats LocalShadowPass::Render(nvrhi::ICommandList *commandList,
                                               std::span<const LocalShadowRequest> requests,
                                               std::span<const ShadowCaster> casters,
                                               const LocalShadowCasterIndex &casterIndex)
{
    Stats stats;
    _tiles.clear();
    if (!IsActive() || commandList == nullptr)
    {
        return stats;
    }
    // The index is a row per request plus a terminator; anything else is a
    // caller that built it against a different request list, and serving it
    // would draw one light's casters into another's tile.
    if (casterIndex.start.size() != requests.size() + 1u)
    {
        Core::Log::Error("LocalShadowPass: the caster index describes {} requests, not {}.",
                         casterIndex.start.empty() ? 0u : casterIndex.start.size() - 1u, requests.size());
        return stats;
    }

    ASSISI_PROFILE_GPU_PASS(commandList, "shadow-atlas");

    stats.unserved = AllocateTiles(requests);
    stats.lights = static_cast<std::uint32_t>(_tiles.size());
    stats.views = static_cast<std::uint32_t>(_targets.size());
    stats.occupancy = _allocator.TotalTexels() == 0
                      ? 0.f
                      : static_cast<float>(_allocator.AllocatedTexels()) /
                      static_cast<float>(_allocator.TotalTexels());

    {
        // The whole atlas at once, not tile by tile: a clear is a fixed cost per
        // rectangle and one covering rectangle is cheaper than sixty. Every tile
        // is redrawn every frame today, so there is nothing in it worth keeping.
        ASSISI_PROFILE_GPU_SCOPE(commandList, "atlas-clear");
        commandList->clearDepthStencilTexture(_atlasTexture, nvrhi::AllSubresources, true, 1.0f, false, 0);
    }

    if (_targets.empty())
    {
        return stats;
    }

    ASSISI_PROFILE_GPU_SCOPE(commandList, "atlas-depth");

    // Whole lights per chunk: a light's faces share one cull, so splitting them
    // across two draw lists would mean walking its casters twice.
    std::uint32_t tile = 0;
    while (tile < _tiles.size())
    {
        const std::uint32_t firstView = _tileViewStart[tile];
        std::uint32_t last = tile;
        while (last < _tiles.size() && _tileViewStart[last + 1u] - firstView <= kShadowViewMaskBits)
        {
            ++last;
        }
        // A single light with more faces than a mask has bits cannot be served
        // at all; six is the most any light asks for, so this is unreachable
        // while kShadowViewMaskBits stays anywhere near its 32.
        if (last == tile)
        {
            break;
        }

        const ShadowDepthRenderer::Stats drawn =
            RenderChunk(commandList, tile, last - tile, firstView, _tileViewStart[last] - firstView, casters,
                        casterIndex);

        // The frame's table is one table across every chunk and every kind of
        // shadow map, so a light's view index is where the renderer put it
        // rather than where this pass counted it.
        for (std::uint32_t moved = tile; moved < last; ++moved)
        {
            _tiles[moved].firstView += drawn.firstView - firstView;
        }

        stats.instances += drawn.instances;
        stats.batches += drawn.batches;
        stats.maskedBatches += drawn.maskedBatches;
        stats.drawCalls += drawn.drawCalls;
        stats.culled += drawn.culled;
        tile = last;
    }

    return stats;
}

} // namespace Assisi::Render
