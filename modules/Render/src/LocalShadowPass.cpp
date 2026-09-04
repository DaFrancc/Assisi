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
    _cacheFramebuffer = nullptr;
    _cacheTexture = nullptr;
    _clearTile = nullptr;
    _builtResolution = 0;
    _builtClearTileSize = 0;
    _clearTileReady = false;
    _builtCacheEnabled = false;
    _tiles.clear();
    _targets.clear();
    // Every remembered rectangle is a rectangle of a texture that is gone, and a
    // tile handed back out of that memory would be depth from nowhere.
    _cache.Forget();
}

bool LocalShadowPass::RebuildCacheTargets()
{
    _cacheFramebuffer = nullptr;
    _cacheTexture = nullptr;
    _clearTile = nullptr;
    _builtClearTileSize = 0;
    _clearTileReady = false;
    _builtCacheEnabled = _settings.cache.enabled;
    // Every rectangle the cache remembered was a rectangle of the texture just
    // dropped, whichever way the toggle moved.
    _cache.Forget();
    if (!_settings.cache.enabled)
    {
        return true;
    }

    nvrhi::TextureDesc desc;
    desc.width = _settings.atlasResolution;
    desc.height = _settings.atlasResolution;
    desc.arraySize = 1;
    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.format = DepthFormat(_settings.format);
    desc.isRenderTarget = true;
    desc.initialState = nvrhi::ResourceStates::DepthWrite;
    desc.keepInitialState = true;
    desc.debugName = "LocalShadowPass::CachedAtlas";

    nvrhi::TextureHandle cache = _device->createTexture(desc);
    if (cache == nullptr)
    {
        Core::Log::Error("LocalShadowPass: failed to allocate the {}x{} cached shadow atlas.",
                         _settings.atlasResolution, _settings.atlasResolution);
        return false;
    }

    nvrhi::FramebufferDesc framebufferDesc;
    framebufferDesc.setDepthAttachment(nvrhi::FramebufferAttachment().setTexture(cache));
    nvrhi::FramebufferHandle framebuffer = _device->createFramebuffer(framebufferDesc);
    if (framebuffer == nullptr)
    {
        Core::Log::Error("LocalShadowPass: failed to create the cached atlas framebuffer.");
        return false;
    }

    // The largest tile the settings can hand out, which is the face resolution
    // the selector treats as a ceiling — never larger, so no rectangle needing a
    // blank is wider than this.
    const std::uint32_t clearSize = std::min(_settings.faceResolution, _settings.atlasResolution);
    nvrhi::TextureDesc clearDesc = desc;
    clearDesc.width = clearSize;
    clearDesc.height = clearSize;
    clearDesc.debugName = "LocalShadowPass::ClearTile";
    nvrhi::TextureHandle clearTile = _device->createTexture(clearDesc);
    if (clearTile == nullptr)
    {
        Core::Log::Error("LocalShadowPass: failed to allocate the {}x{} clear tile.", clearSize, clearSize);
        return false;
    }

    _cacheTexture = std::move(cache);
    _cacheFramebuffer = std::move(framebuffer);
    _clearTile = std::move(clearTile);
    _builtClearTileSize = clearSize;
    return true;
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
    const bool targetsStale =
        _atlasFramebuffer == nullptr || safe.atlasResolution != _builtResolution || safe.format != _builtFormat;
    const bool pipelinesStale =
        _pipelines[static_cast<std::uint32_t>(MeshPipeline::Opaque)] == nullptr || safe.slopeBias != _builtSlopeBias;
    // The kept-depth atlas is the live one's shape, so it follows it, and the
    // clear tile follows the face class it has to be able to blank.
    const bool cacheStale =
        targetsStale || safe.cache.enabled != _builtCacheEnabled ||
        (safe.cache.enabled && std::min(safe.faceResolution, safe.atlasResolution) != _builtClearTileSize);
    _settings = safe;

    if (!targetsStale && !pipelinesStale && !cacheStale)
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
    if (cacheStale && !RebuildCacheTargets())
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
    _targetRequest.clear();
    _targetFace.clear();
    _tileViewStart.clear();
    _tileViewStart.push_back(0);
    _servedRequests.clear();
    _servedRects.clear();

    _allocator.Reset(_settings.atlasResolution);

    // The class each request ends up at, decided across the two passes below: a
    // reserved tile keeps the class its depth was recorded at, and a fresh one
    // takes whatever is left after every reservation is out of the way.
    static constexpr std::uint32_t kUnserved = 0xFFFFFFFFu;
    std::vector<std::uint32_t> assignedClass(requests.size(), kUnserved);

    // Pass one: every tile a light is keeping, at the rectangle it kept. Before
    // any fresh cut, because a fresh cut takes whichever node is free and that
    // node may be the one a later reservation wanted.
    for (std::size_t index = 0; index < requests.size(); ++index)
    {
        const LocalShadowTilePlan &plan = _plans[index];
        if (!plan.retained || plan.deferred)
        {
            continue;
        }
        const std::uint32_t faces = LocalShadowFaceCount(requests[index].kind);
        bool held = true;
        for (std::uint32_t face = 0; face < faces && held; ++face)
        {
            held = _allocator.Reserve(plan.rect[face], requests[index].sizeClass);
        }
        // A partial reservation cannot be given back until the next Reset, so a
        // light that lost even one face keeps the rest rather than stranding
        // them — its remaining faces are still its own depth, and the next frame
        // reconciles. It is simply not served this frame.
        assignedClass[index] = held ? requests[index].sizeClass : kUnserved;
    }

    // Pass two: fresh tiles, demoting until the atlas can serve them.
    for (std::size_t index = 0; index < requests.size(); ++index)
    {
        if (_plans[index].deferred || _plans[index].retained)
        {
            continue;
        }
        const std::uint32_t faces = LocalShadowFaceCount(requests[index].kind);

        // Asked before anything is cut, because a tile handed out cannot be
        // given back until the next Reset — so a point light that ran out on its
        // sixth face would have stranded five.
        std::uint32_t sizeClass = std::min(requests[index].sizeClass, kShadowSizeClassCount - 1u);
        while (_allocator.Available(sizeClass) < faces && sizeClass > 0u)
        {
            --sizeClass;
        }
        if (_allocator.Available(sizeClass) < faces)
        {
            continue;
        }
        assignedClass[index] = sizeClass;
        for (std::uint32_t face = 0; face < faces; ++face)
        {
            _plans[index].rect[face] = _allocator.Allocate(sizeClass);
        }
    }

    // The views, in request order — which is importance order, and which the
    // composite and the cache's residency both read back in.
    std::uint32_t unserved = 0;
    for (std::size_t index = 0; index < requests.size(); ++index)
    {
        const LocalShadowRequest &request = requests[index];
        if (assignedClass[index] == kUnserved)
        {
            // A light the budget refused is not counted here. Both go
            // unshadowed, but they are two different settings to reach for —
            // one wants a bigger atlas, the other a bigger redraw budget — and
            // one number for both would name neither.
            unserved += _plans[index].deferred ? 0u : 1u;
            continue;
        }

        const std::uint32_t faces = LocalShadowFaceCount(request.kind);
        const auto firstView = static_cast<std::uint32_t>(_targets.size());
        std::uint32_t resolution = 0;
        for (std::uint32_t face = 0; face < faces; ++face)
        {
            const ShadowAtlasTile tile{.rect = _plans[index].rect[face], .atlasResolution = _settings.atlasResolution};
            resolution = tile.rect.width;
            const ShadowView view = request.kind == LocalLightKind::Point
                                        ? PointFaceShadowView(request.pose, face, tile, _settings)
                                        : SpotShadowView(request.pose, tile, _settings);
            _targets.push_back(ShadowDepthTarget{.view = view, .framebuffer = _atlasFramebuffer});
            _targetRequest.push_back(static_cast<std::uint32_t>(index));
            _targetFace.push_back(face);
            _servedRects.push_back(tile.rect);
        }

        _tiles.push_back(Tile{
                .kind = request.kind, .lightIndex = request.lightIndex, .firstView = firstView, .resolution = resolution});
        _tileViewStart.push_back(static_cast<std::uint32_t>(_targets.size()));
        _servedRequests.push_back(static_cast<std::uint32_t>(index));
    }
    return unserved;
}

ShadowDepthRenderer::Stats LocalShadowPass::RenderTargets(nvrhi::ICommandList *commandList, const TargetRun &run,
                                                          CasterSide side, const Frame &frame)
{
    ShadowDepthRenderer::Stats total;
    const std::span<const ShadowDepthTarget> targets = run.targets;
    const std::span<const std::uint32_t> targetRequest = run.request;
    if (targets.empty())
    {
        return total;
    }

    const std::span<const ShadowCaster> casters = frame.casters;
    const LocalShadowCasterIndex &casterIndex = *frame.casterIndex;

    bool first = true;
    for (std::uint32_t start = 0; start < targets.size(); start += kShadowViewMaskBits)
    {
        const auto count =
            static_cast<std::uint32_t>(std::min<std::size_t>(kShadowViewMaskBits, targets.size() - start));

        // Stamped rather than cleared: a chunk touches the casters its lights
        // reach, and clearing a span sized to the whole scene for each chunk
        // would cost more than the chunk does.
        ++_chunkStamp;
        _casterMask.resize(casters.size());
        _casterStamp.resize(casters.size(), 0);

        for (std::uint32_t local = 0; local < count; ++local)
        {
            // A view's bit is its position inside this chunk, which is what
            // keeps a mask inside kShadowViewMaskBits however many views the
            // frame draws in total.
            const std::uint32_t bit = 1u << local;
            const std::uint32_t request = targetRequest[start + local];
            const std::uint32_t rowEnd = casterIndex.start[request + 1u];
            for (std::uint32_t entry = casterIndex.start[request]; entry < rowEnd; ++entry)
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
                _casterMask[index] |= bit;
            }
        }

        // One sweep in the caster span's own order, so the chunk's list stays
        // sorted by pipeline class and geometry key exactly as the span is —
        // which is what lets consecutive entries still coalesce into one
        // instanced draw.
        _chunkCasters.clear();
        for (std::uint32_t index = 0; index < casters.size(); ++index)
        {
            if (_casterStamp[index] != _chunkStamp || _casterMask[index] == 0u)
            {
                continue;
            }
            // The cached/dynamic split, and the only place it decides anything:
            // the two passes walk the same rows and keep opposite halves, so a
            // caster is in the kept layer or over it and never in both.
            const ShadowCasterMotion motion = casters[index].motion;
            if ((side == CasterSide::Static && motion != ShadowCasterMotion::Still) ||
                (side == CasterSide::Dynamic && motion != ShadowCasterMotion::Moving))
            {
                continue;
            }
            ShadowCaster caster = casters[index];
            caster.viewMask = _casterMask[index];
            _chunkCasters.push_back(caster);
        }

        const ShadowDepthRenderer::Stats drawn =
            _depthRenderer->Render(commandList, PipelineSet(), targets.subspan(start, count), _chunkCasters);
        if (first)
        {
            total.firstView = drawn.firstView;
            first = false;
        }
        total.views += drawn.views;
        total.instances += drawn.instances;
        total.batches += drawn.batches;
        total.maskedBatches += drawn.maskedBatches;
        total.drawCalls += drawn.drawCalls;
        total.culled += drawn.culled;
    }
    return total;
}

void LocalShadowPass::BlankCacheTile(nvrhi::ICommandList *commandList, const ShadowViewRect &rect)
{
    if (_clearTile == nullptr || rect.width == 0u)
    {
        return;
    }
    if (rect.width > _builtClearTileSize)
    {
        // Unreachable while the selector treats the face resolution as a
        // ceiling, which is what the clear tile is sized from. Said out loud
        // rather than skipped, because a rectangle that cannot be blanked is one
        // re-baked over depth that is still in it — a stale shadow, and the one
        // defect here with no visual tell.
        Core::Log::Error("LocalShadowPass: a {}-texel tile cannot be blanked by a {}-texel clear tile.", rect.width,
                         _builtClearTileSize);
        return;
    }
    if (!_clearTileReady)
    {
        commandList->clearDepthStencilTexture(_clearTile, nvrhi::AllSubresources, true, 1.0f, false, 0);
        _clearTileReady = true;
    }
    const auto source = nvrhi::TextureSlice().setOrigin(0, 0, 0).setSize(rect.width, rect.height, 1);
    const auto destination = nvrhi::TextureSlice().setOrigin(rect.x, rect.y, 0).setSize(rect.width, rect.height, 1);
    commandList->copyTexture(_cacheTexture, destination, _clearTile, source);
}

void LocalShadowPass::CopyCachedTile(nvrhi::ICommandList *commandList, const ShadowViewRect &rect)
{
    if (_cacheTexture == nullptr || rect.width == 0u)
    {
        return;
    }
    // The same rectangle of both textures, which is why the kept layer is its
    // own atlas of the same shape rather than a region of one.
    const auto slice = nvrhi::TextureSlice().setOrigin(rect.x, rect.y, 0).setSize(rect.width, rect.height, 1);
    commandList->copyTexture(_atlasTexture, slice, _cacheTexture, slice);
}

LocalShadowPass::Stats LocalShadowPass::RenderUncached(nvrhi::ICommandList *commandList, const Frame &frame,
                                                       Stats stats)
{
    {
        // The whole atlas at once, not tile by tile: a clear is a fixed cost per
        // rectangle and one covering rectangle is cheaper than sixty. Nothing is
        // kept on this path, so there is nothing in it worth keeping.
        ASSISI_PROFILE_GPU_SCOPE(commandList, "atlas-clear");
        commandList->clearDepthStencilTexture(_atlasTexture, nvrhi::AllSubresources, true, 1.0f, false, 0);
    }
    if (_targets.empty())
    {
        return stats;
    }

    ASSISI_PROFILE_GPU_SCOPE(commandList, "atlas-depth");
    const ShadowDepthRenderer::Stats drawn =
        RenderTargets(commandList, TargetRun{.targets = _targets, .request = _targetRequest}, CasterSide::Both, frame);

    // The frame's table is one table across every kind of shadow map, so a
    // light's view index is where the renderer put it rather than where this
    // pass counted it. The views were appended in target order, so one offset
    // relocates every tile.
    for (Tile &tile : _tiles)
    {
        tile.firstView += drawn.firstView;
    }

    stats.instances = drawn.instances;
    stats.batches = drawn.batches;
    stats.maskedBatches = drawn.maskedBatches;
    stats.drawCalls = drawn.drawCalls;
    stats.culled = drawn.culled;
    return stats;
}

LocalShadowPass::Stats LocalShadowPass::RenderCached(nvrhi::ICommandList *commandList, const Frame &frame, Stats stats)
{
    // Which faces need their kept layer redrawn, and which need composing from
    // it. A face needs composing when its kept layer just changed, or when
    // something is moving over it — and neither, which is the resting case, is
    // no work at all.
    _bakeTargets.clear();
    _bakeTargetRequest.clear();
    for (std::uint32_t target = 0; target < _targets.size(); ++target)
    {
        const LocalShadowTilePlan &plan = _plans[_targetRequest[target]];
        if ((plan.dirtyFaces & (1u << _targetFace[target])) == 0u)
        {
            continue;
        }
        ShadowDepthTarget baked = _targets[target];
        baked.framebuffer = _cacheFramebuffer;
        _bakeTargets.push_back(baked);
        _bakeTargetRequest.push_back(_targetRequest[target]);
    }

    if (!_bakeTargets.empty())
    {
        ASSISI_PROFILE_GPU_SCOPE(commandList, "atlas-bake");
        for (const ShadowDepthTarget &target : _bakeTargets)
        {
            // Blanked one rectangle at a time. Clearing the attachment would
            // wipe every other light's kept depth to re-bake one, which is the
            // opposite of what the cache is for.
            BlankCacheTile(commandList, target.view.rect);
        }
        const ShadowDepthRenderer::Stats baked = RenderTargets(
            commandList, TargetRun{.targets = _bakeTargets, .request = _bakeTargetRequest}, CasterSide::Static, frame);
        stats.bakedFaces = static_cast<std::uint32_t>(_bakeTargets.size());
        stats.instances += baked.instances;
        stats.batches += baked.batches;
        stats.maskedBatches += baked.maskedBatches;
        stats.drawCalls += baked.drawCalls;
        stats.culled += baked.culled;
    }

    if (!_targets.empty())
    {
        ASSISI_PROFILE_GPU_SCOPE(commandList, "atlas-compose");
        for (std::uint32_t target = 0; target < _targets.size(); ++target)
        {
            const LocalShadowTilePlan &plan = _plans[_targetRequest[target]];
            const bool rebaked = (plan.dirtyFaces & (1u << _targetFace[target])) != 0u;
            const bool moving = plan.hasMovers && plan.redrawMovers;
            if (!rebaked && !moving)
            {
                continue; // the live tile already holds exactly this
            }
            CopyCachedTile(commandList, _targets[target].view.rect);
            ++stats.copiedFaces;
        }
    }

    // Every served face, every frame, whether or not anything draws into it: the
    // frame's view table is what the mesh shader reads a tile through, so a
    // resting light needs a row in it as much as a redrawn one does. A resting
    // light's row names no casters, so this costs the table and no draw.
    ASSISI_PROFILE_GPU_SCOPE(commandList, "atlas-dynamic");
    const ShadowDepthRenderer::Stats drawn = RenderTargets(
        commandList, TargetRun{.targets = _targets, .request = _targetRequest}, CasterSide::Dynamic, frame);
    for (Tile &tile : _tiles)
    {
        tile.firstView += drawn.firstView;
    }

    stats.instances += drawn.instances;
    stats.batches += drawn.batches;
    stats.maskedBatches += drawn.maskedBatches;
    stats.drawCalls += drawn.drawCalls;
    stats.culled += drawn.culled;
    return stats;
}

bool LocalShadowPass::PlanFrame(const Frame &frame)
{
    _planned = true;
    _plannedFrame = frame.frameIndex;

    if (!IsActive())
    {
        _plans.clear();
        return false;
    }

    if (_settings.cache.enabled && _cacheFramebuffer != nullptr)
    {
        _cache.Plan(LocalShadowCacheFrame{.frameIndex = frame.frameIndex,
                                          .settings = _settings.cache,
                                          .requests = frame.requests,
                                          .movers = frame.movers,
                                          .invalidations = frame.invalidations},
                    _plans);
    }
    else
    {
        // Nothing is kept, so every light asks for a fresh tile and every face
        // is drawn — which is the pass exactly as it was before there was a
        // cache, and the baseline the cached path is measured against.
        _plans.assign(frame.requests.size(), LocalShadowTilePlan{});
    }

    // A tile with a dirty face needs the *still* casters, which have not moved
    // and are not in the mover set — a light that moved, or came back from not
    // casting, dirties its faces without anything else in the scene stirring.
    // Answering this from the movers alone bakes those tiles from an empty
    // caster list, which blanks them and leaves that light lit with no shadow
    // at all until something else happens to dirty it again.
    for (const LocalShadowTilePlan &plan : _plans)
    {
        if (plan.dirtyFaces != 0u || (plan.hasMovers && plan.redrawMovers))
        {
            return true;
        }
    }
    return false;
}

LocalShadowPass::Stats LocalShadowPass::Render(nvrhi::ICommandList *commandList, const Frame &frame)
{
    Stats stats;
    _tiles.clear();
    if (!IsActive() || commandList == nullptr || frame.casterIndex == nullptr)
    {
        return stats;
    }
    // The index is a row per request plus a terminator; anything else is a
    // caller that built it against a different request list, and serving it
    // would draw one light's casters into another's tile.
    if (frame.casterIndex->start.size() != frame.requests.size() + 1u)
    {
        Core::Log::Error("LocalShadowPass: the caster index describes {} requests, not {}.",
                         frame.casterIndex->start.empty() ? 0u : frame.casterIndex->start.size() - 1u,
                         frame.requests.size());
        return stats;
    }

    ASSISI_PROFILE_GPU_PASS(commandList, "shadow-atlas");

    const bool caching = _settings.cache.enabled && _cacheFramebuffer != nullptr;
    if (!_planned || _plannedFrame != frame.frameIndex)
    {
        (void)PlanFrame(frame);
    }
    _planned = false;

    stats.unserved = AllocateTiles(frame.requests);
    stats.lights = static_cast<std::uint32_t>(_tiles.size());
    stats.views = static_cast<std::uint32_t>(_targets.size());
    stats.occupancy = _allocator.TotalTexels() == 0 ? 0.f
                                                    : static_cast<float>(_allocator.AllocatedTexels()) /
                      static_cast<float>(_allocator.TotalTexels());

    stats = caching ? RenderCached(commandList, frame, stats) : RenderUncached(commandList, frame, stats);

    if (caching)
    {
        _cache.Commit(frame.frameIndex, frame.requests, _plans, _servedRequests, _servedRects);
        const LocalShadowCachePlanStats &planned = _cache.Stats();
        stats.restingLights = planned.restingLights;
        stats.deferredLights = planned.deferredLights;
        stats.dynamicCasters = planned.dynamicCasters;
    }
    return stats;
}

} // namespace Assisi::Render
