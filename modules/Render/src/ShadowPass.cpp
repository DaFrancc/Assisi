/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ShadowPass.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/GpuMarker.hpp>
#include <Assisi/Render/ShaderModule.hpp>

#include <algorithm>

namespace Assisi::Render
{
namespace
{
/// Mirrors shadow_depth_restore.frag's push-constant block.
struct RestorePush
{
    std::uint32_t cascade = 0;
    std::uint32_t padding0 = 0;
    std::uint32_t padding1 = 0;
    std::uint32_t padding2 = 0;
};

/// The restore draw's triangle, which covers whatever viewport it is given.
constexpr std::uint32_t kRestoreTriangleVertices = 3;

nvrhi::Format DepthFormat(ShadowMapFormat format)
{
    return format == ShadowMapFormat::D16 ? nvrhi::Format::D16 : nvrhi::Format::D32;
}

/// Cascade texels on a side of one tile of the record of where movers were
/// drawn. Coarse enough that putting a mover's texels back is a handful of
/// draws, fine enough that it rewrites little more than the mover covered.
constexpr std::uint32_t kMoverTileTexels = 64;

/// Extra texels around a mover's footprint: its sphere's projection is exact,
/// and one texel covers the rasterizer's coverage of the edge it straddles.
constexpr float kMoverFootprintMarginTexels = 1.f;

std::uint32_t MoverTilesPerSide(std::uint32_t resolution)
{
    return (resolution + kMoverTileTexels - 1u) / kMoverTileTexels;
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

    _restoreVertexShader = LoadSpirvShader(_device, params.restoreVertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    _restorePixelShader = LoadSpirvShader(_device, params.restorePixelShaderSpvPath, nvrhi::ShaderType::Pixel);
    nvrhi::BindingLayoutDesc restoreLayout;
    restoreLayout.visibility = nvrhi::ShaderType::Pixel;
    restoreLayout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    restoreLayout.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RestorePush)));
    _restoreLayout = _device->createBindingLayout(restoreLayout);
    if (_restoreVertexShader == nullptr || _restorePixelShader == nullptr || _restoreLayout == nullptr)
    {
        Core::Log::Error("ShadowPass: failed to load the still-depth restore shaders.");
        return false;
    }
    return CreateNoCascadesTexture();
}

bool ShadowPass::CreateNoCascadesTexture()
{
    nvrhi::TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.arraySize = 1;
    desc.dimension = nvrhi::TextureDimension::Texture2DArray;
    desc.format = nvrhi::Format::D32;
    desc.isShaderResource = true;
    // Never drawn into, but a depth-format image is sampled from the depth
    // read-only layout, and Vulkan admits an image to that layout only if it
    // was created usable as a depth attachment.
    desc.isRenderTarget = true;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName = "ShadowPass::NoCascades";
    _noCascadesTexture = _device->createTexture(desc);
    if (_noCascadesTexture == nullptr)
    {
        Core::Log::Error("ShadowPass: failed to create the empty cascade texture.");
        return false;
    }
    _cascadeTexture = _noCascadesTexture;
    return true;
}

void ShadowPass::ReleaseTargets()
{
    _cascadeFramebuffers.clear();
    _stillFramebuffers.clear();
    _cascadeTexture = _noCascadesTexture;
    _stillTexture = nullptr;
    _restoreSet = nullptr;
    _restorePipeline = nullptr;
    for (std::vector<std::uint8_t> &tiles : _moverTiles)
    {
        tiles.clear();
    }
    _builtCascades = 0;
    _builtResolution = 0;
    ++_allocationGeneration;
}

bool ShadowPass::RebuildTargets()
{
    _cascadeFramebuffers.clear();
    _stillFramebuffers.clear();

    const auto makeArray = [this](const char *name)
                           {
                               nvrhi::TextureDesc desc;
                               desc.width = _settings.resolution;
                               desc.height = _settings.resolution;
                               desc.arraySize = _settings.cascadeCount;
                               desc.dimension = nvrhi::TextureDimension::Texture2DArray;
                               desc.format = DepthFormat(_settings.format);
                               desc.isRenderTarget = true;
                               desc.isShaderResource = true;
                               // Written as depth, copied between, and read as an
                               // SRV in the same frame; nvrhi's automatic barriers
                               // move it between them and restore this after.
                               desc.initialState = nvrhi::ResourceStates::DepthWrite;
                               desc.keepInitialState = true;
                               desc.debugName = name;
                               return _device->createTexture(desc);
                           };
    nvrhi::TextureHandle texture = makeArray("ShadowPass::Cascades");
    nvrhi::TextureHandle still = makeArray("ShadowPass::StillCascades");
    if (texture == nullptr || still == nullptr)
    {
        Core::Log::Error("ShadowPass: failed to allocate the {}x{} x{} cascade arrays.", _settings.resolution,
                         _settings.resolution, _settings.cascadeCount);
        return false;
    }

    // One framebuffer per slice rather than one layered framebuffer: each
    // cascade is drawn with its own matrix, so there is nothing for a
    // layered pass to amortise without a geometry stage this does not have.
    const auto makeFramebuffers = [this](nvrhi::ITexture *array, std::vector<nvrhi::FramebufferHandle> &out)
                                  {
                                      out.reserve(_settings.cascadeCount);
                                      for (std::uint32_t i = 0; i < _settings.cascadeCount; ++i)
                                      {
                                          nvrhi::FramebufferDesc framebufferDesc;
                                          framebufferDesc.setDepthAttachment(
                                              nvrhi::FramebufferAttachment().setTexture(array).setArraySlice(
                                                  static_cast<nvrhi::ArraySlice>(i)));
                                          nvrhi::FramebufferHandle framebuffer =
                                              _device->createFramebuffer(framebufferDesc);
                                          if (framebuffer == nullptr)
                                          {
                                              Core::Log::Error(
                                                  "ShadowPass: failed to create the framebuffer for cascade {}.", i);
                                              out.clear();
                                              return false;
                                          }
                                          out.push_back(std::move(framebuffer));
                                      }
                                      return true;
                                  };
    if (!makeFramebuffers(texture, _cascadeFramebuffers) || !makeFramebuffers(still, _stillFramebuffers))
    {
        _cascadeFramebuffers.clear();
        _stillFramebuffers.clear();
        return false;
    }

    nvrhi::BindingSetDesc restoreSet;
    restoreSet.addItem(nvrhi::BindingSetItem::Texture_SRV(0, still));
    restoreSet.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(RestorePush)));
    _restoreSet = _device->createBindingSet(restoreSet, _restoreLayout);
    if (_restoreSet == nullptr)
    {
        Core::Log::Error("ShadowPass: failed to create the still-depth restore binding set.");
        _cascadeFramebuffers.clear();
        _stillFramebuffers.clear();
        return false;
    }

    _cascadeTexture = std::move(texture);
    _stillTexture = std::move(still);
    const std::uint32_t tilesPerSide = MoverTilesPerSide(_settings.resolution);
    for (std::vector<std::uint8_t> &tiles : _moverTiles)
    {
        tiles.assign(static_cast<std::size_t>(tilesPerSide) * tilesPerSide, 0u);
    }
    _builtCascades = _settings.cascadeCount;
    _builtResolution = _settings.resolution;
    _builtFormat = _settings.format;
    ++_allocationGeneration;
    return true;
}

bool ShadowPass::RebuildPipeline()
{
    if (_cascadeFramebuffers.empty())
    {
        return false;
    }

    // Sized against the map's texel, so raising the resolution narrows the gap
    // the slope bias can open instead of widening it.
    const float slopeBiasClamp = SlopeBiasClampNdc(_settings);

    for (std::uint32_t index = 0; index < kMeshPipelineCount; ++index)
    {
        _pipelines[index] = _depthRenderer->CreatePipeline(_cascadeFramebuffers.front(),
                                                           static_cast<MeshPipeline>(index), _settings.slopeBias,
                                                           slopeBiasClamp, ShadowProjection::Orthographic);
    }
    // The opaque single-sided class is the one nothing can do without: it is
    // what every other class falls back to. A null masked entry is not a
    // failure — the renderer simply carries no alpha-testing variant, and the
    // cascades still render with cutouts casting their full silhouette.
    if (_pipelines[static_cast<std::uint32_t>(MeshPipeline::Opaque)] == nullptr)
    {
        return false;
    }

    // Depth only, written unconditionally: the still depth replaces whatever
    // a mover left, nearer or not.
    nvrhi::GraphicsPipelineDesc restoreDesc;
    restoreDesc.primType = nvrhi::PrimitiveType::TriangleList;
    restoreDesc.VS = _restoreVertexShader;
    restoreDesc.PS = _restorePixelShader;
    restoreDesc.bindingLayouts = {_restoreLayout};
    restoreDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    restoreDesc.renderState.depthStencilState.depthTestEnable = true;
    restoreDesc.renderState.depthStencilState.depthWriteEnable = true;
    restoreDesc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::Always;
    _restorePipeline = _device->createGraphicsPipeline(restoreDesc, _cascadeFramebuffers.front()->getFramebufferInfo());
    if (_restorePipeline == nullptr)
    {
        Core::Log::Error("ShadowPass: failed to create the still-depth restore pipeline.");
        return false;
    }
    _builtSlopeBias = _settings.slopeBias;
    return true;
}

void ShadowPass::RestoreStillDepth(nvrhi::ICommandList *commandList, std::uint32_t cascade,
                                   const nvrhi::Rect &region) const
{
    nvrhi::GraphicsState state;
    state.pipeline = _restorePipeline;
    state.framebuffer = _cascadeFramebuffers[cascade];
    state.bindings = {_restoreSet};
    state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(region.minX),
                                                             static_cast<float>(region.maxX),
                                                             static_cast<float>(region.minY),
                                                             static_cast<float>(region.maxY), 0.f, 1.f));
    commandList->setGraphicsState(state);
    const RestorePush push{.cascade = cascade};
    commandList->setPushConstants(&push, sizeof(push));
    nvrhi::DrawArguments triangle;
    triangle.vertexCount = kRestoreTriangleVertices;
    commandList->draw(triangle);
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
            _pipelines = {};
            _active = false;
        }
        return true;
    }

    const SunShadowSettings safe = Sanitized(settings);
    const bool targetsStale = _cascadeFramebuffers.empty() || safe.cascadeCount != _builtCascades ||
                              safe.resolution != _builtResolution || safe.format != _builtFormat;
    const bool pipelineStale = _pipelines[static_cast<std::uint32_t>(MeshPipeline::Opaque)] == nullptr ||
                               safe.slopeBias != _builtSlopeBias;
    _settings = safe;

    if (!targetsStale && !pipelineStale)
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
    // A new array means new framebuffers, and a pipeline is built against one.
    if ((targetsStale || pipelineStale) && !RebuildPipeline())
    {
        ReleaseTargets();
        _pipelines = {};
        _active = false;
        return false;
    }

    _active = true;
    return true;
}

ShadowPipelines ShadowPass::PipelineSet() const
{
    ShadowPipelines set;
    for (std::uint32_t index = 0; index < kMeshPipelineCount; ++index)
    {
        set.byPipeline[index] = _pipelines[index];
    }
    return set;
}

ShadowPass::Stats ShadowPass::Render(nvrhi::ICommandList *commandList, const SunShadowCadencePlan &plan,
                                     std::span<const ShadowCaster> casters) const
{
    Stats stats;
    const CascadeFit &fit = plan.fit;
    if (!IsActive() || commandList == nullptr || fit.count == 0)
    {
        return stats;
    }

    const std::uint32_t cascadeCount = std::min<std::uint32_t>(fit.count, _builtCascades);
    const std::span<const std::uint32_t> redraw(plan.redraw.data(), plan.redrawCount);
    const std::span<const std::uint32_t> movingRedraw(plan.movingRedraw.data(), plan.movingRedrawCount);
    for (const std::uint32_t cascade : redraw)
    {
        if (cascade >= cascadeCount)
        {
            return stats; // a plan for an allocation this is not
        }
    }
    for (const std::uint32_t cascade : movingRedraw)
    {
        if (cascade >= cascadeCount)
        {
            return stats;
        }
    }

    ASSISI_PROFILE_GPU_PASS(commandList, "shadow-cascades");

    // Each draw below numbers its views from zero, so each gets the casters
    // whose bits name its views, shifted to start there.
    const std::uint32_t stillBits = ShadowViewBits(static_cast<std::uint32_t>(redraw.size()));
    _stillCasters.clear();
    _movingCasters.clear();
    for (const ShadowCaster &caster : casters)
    {
        if ((caster.viewMask & stillBits) != 0u)
        {
            ShadowCaster still = caster;
            still.viewMask &= stillBits;
            _stillCasters.push_back(still);
        }
        const std::uint32_t movingMask =
            ShadowViewBitsAfter(caster.viewMask, static_cast<std::uint32_t>(redraw.size()));
        if (movingMask != 0u)
        {
            ShadowCaster moving = caster;
            moving.viewMask = movingMask;
            _movingCasters.push_back(moving);
        }
    }

    // The still layers: cleared because they are about to be redrawn, and only
    // then — a layer this frame is keeping holds depth the fit still describes.
    ShadowDepthRenderer::Stats still;
    {
        ASSISI_PROFILE_GPU_SCOPE(commandList, "cascade-still");
        _scratchTargets.clear();
        for (const std::uint32_t cascade : redraw)
        {
            commandList->clearDepthStencilTexture(
                _stillTexture, nvrhi::TextureSubresourceSet(0, 1, static_cast<nvrhi::ArraySlice>(cascade), 1), true,
                1.0f, false, 0);
            _scratchTargets.push_back(
                ShadowDepthTarget{.view = CascadeShadowView(fit.cascades[cascade], cascade, _settings),
                                  .framebuffer = _stillFramebuffers[cascade]});
        }
        if (!_scratchTargets.empty())
        {
            still = _depthRenderer->Render(commandList, PipelineSet(), _scratchTargets, _stillCasters);
        }
    }
    // Read back by the index each cascade was *submitted* at, then filed under
    // the cascade it was: the two differ whenever a cascade is being kept.
    if (_cascadeCounts && !_scratchTargets.empty())
    {
        const ShadowDrawList &list = _depthRenderer->LastDrawList();
        for (std::uint32_t target = 0; target < _scratchTargets.size(); ++target)
        {
            stats.cascadeCasters[_scratchTargets[target].view.arraySlice] = ShadowViewCasterCount(list, target);
        }
    }

    // The slices the shader reads: the still depth, with the movers over it.
    {
        ASSISI_PROFILE_GPU_SCOPE(commandList, "cascade-restore");
        const std::int32_t side = static_cast<std::int32_t>(_builtResolution);
        // Stated outright rather than left to the binding set: nvrhi re-derives
        // a set's barriers only when the bound sets change or something marks
        // them dirty, and a depth clear does not. A restore whose set is
        // already current would read the still layer with no barrier after
        // the clear and still draw that just wrote it.
        commandList->setTextureState(_stillTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->commitBarriers();
        for (const std::uint32_t cascade : redraw)
        {
            RestoreStillDepth(commandList, cascade, nvrhi::Rect(0, side, 0, side));
            std::ranges::fill(_moverTiles[cascade], std::uint8_t{0});
        }
        for (const std::uint32_t cascade : movingRedraw)
        {
            if (std::ranges::find(redraw, cascade) == redraw.end())
            {
                RestoreMoverTiles(commandList, cascade);
            }
        }
    }

    ShadowDepthRenderer::Stats moving;
    {
        ASSISI_PROFILE_GPU_SCOPE(commandList, "cascade-movers");
        _scratchTargets.clear();
        for (const std::uint32_t cascade : movingRedraw)
        {
            _scratchTargets.push_back(
                ShadowDepthTarget{.view = CascadeShadowView(fit.cascades[cascade], cascade, _settings),
                                  .framebuffer = _cascadeFramebuffers[cascade]});
        }
        if (!_scratchTargets.empty() && !_movingCasters.empty())
        {
            moving = _depthRenderer->Render(commandList, PipelineSet(), _scratchTargets, _movingCasters);
        }
        RecordMoverTiles(fit, movingRedraw);
    }

    _firstView = redraw.empty() ? moving.firstView : still.firstView;
    stats.cascades = static_cast<std::uint32_t>(redraw.size());
    stats.cascadesKept = cascadeCount - std::min(cascadeCount, stats.cascades);
    stats.moverCascades = static_cast<std::uint32_t>(movingRedraw.size());
    stats.instances = still.instances + moving.instances;
    stats.batches = still.batches + moving.batches;
    stats.maskedBatches = still.maskedBatches + moving.maskedBatches;
    stats.drawCalls = still.drawCalls + moving.drawCalls;
    stats.culled = still.culled + moving.culled;
    return stats;
}

void ShadowPass::RestoreMoverTiles(nvrhi::ICommandList *commandList, std::uint32_t cascade) const
{
    std::vector<std::uint8_t> &tiles = _moverTiles[cascade];
    const std::uint32_t tilesPerSide = MoverTilesPerSide(_builtResolution);
    if (tiles.size() != static_cast<std::size_t>(tilesPerSide) * tilesPerSide)
    {
        return;
    }
    // A run of marked tiles along a row is one copy, which keeps a mover a few
    // tiles across to a few copies rather than one per tile.
    for (std::uint32_t row = 0; row < tilesPerSide; ++row)
    {
        std::uint32_t column = 0;
        while (column < tilesPerSide)
        {
            if (tiles[row * tilesPerSide + column] == 0u)
            {
                ++column;
                continue;
            }
            const std::uint32_t first = column;
            while (column < tilesPerSide && tiles[row * tilesPerSide + column] != 0u)
            {
                tiles[row * tilesPerSide + column] = 0u;
                ++column;
            }
            const std::uint32_t x = first * kMoverTileTexels;
            const std::uint32_t y = row * kMoverTileTexels;
            const std::uint32_t right = std::min(column * kMoverTileTexels, _builtResolution);
            const std::uint32_t bottom = std::min(y + kMoverTileTexels, _builtResolution);
            RestoreStillDepth(commandList, cascade,
                              nvrhi::Rect(static_cast<std::int32_t>(x), static_cast<std::int32_t>(right),
                                          static_cast<std::int32_t>(y), static_cast<std::int32_t>(bottom)));
        }
    }
}

void ShadowPass::RecordMoverTiles(const CascadeFit &fit, std::span<const std::uint32_t> movingRedraw) const
{
    const std::uint32_t tilesPerSide = MoverTilesPerSide(_builtResolution);
    const float resolution = static_cast<float>(_builtResolution);
    for (const ShadowCaster &caster : _movingCasters)
    {
        for (std::uint32_t bit = 0; bit < movingRedraw.size(); ++bit)
        {
            if ((caster.viewMask & (1u << bit)) == 0u)
            {
                continue;
            }
            const std::uint32_t cascade = movingRedraw[bit];
            const ShadowCascade &view = fit.cascades[cascade];
            std::vector<std::uint8_t> &tiles = _moverTiles[cascade];
            if (tiles.size() != static_cast<std::size_t>(tilesPerSide) * tilesPerSide || !(view.worldUnitsPerTexel > 0.f))
            {
                continue;
            }
            // Orthographic, so the sphere's footprint in the map is a disc of
            // its own radius around where its centre lands, whatever its depth.
            // The map's first row is ndc.y = +1, as the lookups have it.
            const glm::vec4 clip = view.viewProjection * glm::vec4(caster.worldSphere.center, 1.f);
            const glm::vec2 texel = (glm::vec2(clip.x, -clip.y) * 0.5f + 0.5f) * resolution;
            const float reach = caster.worldSphere.radius / view.worldUnitsPerTexel + kMoverFootprintMarginTexels;
            const glm::vec2 low = glm::clamp(texel - reach, glm::vec2(0.f), glm::vec2(resolution - 1.f));
            const glm::vec2 high = glm::clamp(texel + reach, glm::vec2(0.f), glm::vec2(resolution - 1.f));
            if (texel.x + reach < 0.f || texel.y + reach < 0.f || texel.x - reach > resolution ||
                texel.y - reach > resolution)
            {
                continue; // drawn, but lands nowhere in the map
            }
            const std::uint32_t firstColumn = static_cast<std::uint32_t>(low.x) / kMoverTileTexels;
            const std::uint32_t lastColumn = static_cast<std::uint32_t>(high.x) / kMoverTileTexels;
            const std::uint32_t firstRow = static_cast<std::uint32_t>(low.y) / kMoverTileTexels;
            const std::uint32_t lastRow = static_cast<std::uint32_t>(high.y) / kMoverTileTexels;
            for (std::uint32_t row = firstRow; row <= lastRow; ++row)
            {
                for (std::uint32_t column = firstColumn; column <= lastColumn; ++column)
                {
                    tiles[row * tilesPerSide + column] = 1u;
                }
            }
        }
    }
}

} // namespace Assisi::Render
