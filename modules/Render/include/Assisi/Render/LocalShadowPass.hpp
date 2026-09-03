/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file LocalShadowPass.hpp
/// @brief One shared atlas for every spot and point light's shadow, and the
/// strategy that fills it.
///
/// A spot light spends one tile of the atlas and a point light six, and both
/// draw through the same depth renderer the sun's cascades do. What is specific
/// to local lights, and so lives here, is the allocation: how the atlas is cut
/// up, which lights get a tile, and what happens when there are more of them
/// than there is texture.
///
/// Nothing is allocated until a shadow-casting local light exists. Configure(...,
/// active = false) drops the atlas, the framebuffer and the pipelines, leaving a
/// one-texel empty atlas so the mesh pass's binding set still has something to
/// point at — the same discipline the cascade array keeps, for the same
/// blank-scene reason.
///
/// **Why the views are drawn in chunks.** A caster's ShadowCaster::viewMask is
/// kShadowViewMaskBits wide, so one draw-list build can serve at most that many
/// views. The Ultra tier's caps reach 64 spots and 16 points, which is 160 views
/// — well past it. The pass therefore packs whole lights into chunks of at most
/// that many views and builds one draw list per chunk. Views stay contiguous in
/// the frame's table across a chunk boundary, because the table is appended to,
/// so a light's faces are consecutive whichever chunk they landed in.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ShadowAtlas.hpp>
#include <Assisi/Render/ShadowDepthRenderer.hpp>
#include <Assisi/Render/ShadowImportance.hpp>
#include <Assisi/Render/ShadowSettings.hpp>
#include <Assisi/Render/ShadowView.hpp>

namespace Assisi::Render
{

/// @brief One light that won tiles this frame, with everything its views are
/// built from.
///
/// The geometry rather than the light component: the pass has no opinion about
/// where a spot's aim came from, only that it points somewhere.
struct LocalShadowRequest
{
    LocalLightKind kind = LocalLightKind::Spot;
    /// Row in this kind's GPU light buffer, carried through so the caller can
    /// stamp the resulting view index back into the light the shader reads.
    std::uint32_t lightIndex = 0;

    glm::vec3 position{0.f};
    /// Where a spot aims, in world space and already normalised. Unread for a
    /// point light, which aims in all six directions.
    glm::vec3 direction{0.f, -1.f, 0.f};
    /// The light's influence radius, which is also its shadow map's far plane:
    /// nothing past it is lit, so nothing past it needs to occlude.
    float range = 1.f;
    /// Half-angle of the spot's outer cone, in degrees. Unread for a point light.
    float outerAngleDegrees = 45.f;

    /// The tile size the selector asked for. A ceiling — the allocator demotes
    /// from here when the atlas cannot serve it.
    std::uint32_t sizeClass = 0;
};

/// @brief Which casters reach which requests, as a compressed row per request.
///
/// Request `i` owns `caster[start[i] .. start[i + 1])`, each entry an index into
/// the caster span. Built once per frame against each light's bounding sphere,
/// which is the "cull once per light" half — the per-face refinement is the
/// frustum test the draw list already makes, so six faces cost one caster walk
/// rather than six.
struct LocalShadowCasterIndex
{
    /// requests.size() + 1 entries, the last holding the total.
    std::vector<std::uint32_t> start;
    std::vector<std::uint32_t> caster;

    void Clear();
};

class LocalShadowPass
{
public:
    LocalShadowPass() = default;

    struct InitParams
    {
        nvrhi::IDevice *device = nullptr;
        /// The shared depth renderer this pass draws through — the same one the
        /// cascades use, so every shadow view of the frame lands in one table.
        /// Not owned, and it must outlive the pass.
        const ShadowDepthRenderer *depthRenderer = nullptr;
    };

    /// @brief Bind to the device and the shared renderer, and create the empty
    /// atlas. The pipelines and the real texture wait for Configure().
    [[nodiscard]] bool Initialize(const InitParams &params);

    /// @brief Bring the pass in line with @p settings.
    ///
    /// @p active is whether anything wants local shadows this frame — settings
    /// enabled *and* a shadow-casting spot or point light in the scene. False
    /// releases the atlas, the framebuffer and the pipelines.
    [[nodiscard]] bool Configure(const LocalShadowSettings &settings, bool active);

    /// @brief Where one light's views landed in the frame's shadow view table.
    struct Tile
    {
        LocalLightKind kind = LocalLightKind::Spot;
        std::uint32_t lightIndex = 0;
        /// Index of this light's first view. Its faces are the next
        /// LocalShadowFaceCount(kind) entries, consecutively.
        std::uint32_t firstView = 0;
        /// The tile edge the light actually got, which is the class it asked for
        /// or a demoted one.
        std::uint32_t resolution = 0;
    };

    /// @brief What one Render() drew.
    struct Stats
    {
        std::uint32_t lights = 0; ///< Lights that got tiles.
        std::uint32_t views = 0;  ///< Atlas faces rendered — spots once, points six times.
        /// Lights the atlas could not serve even at the smallest class. Their
        /// shadow is dropped rather than shrunk, and they light unshadowed.
        std::uint32_t unserved = 0;
        std::uint32_t instances = 0;
        std::uint32_t batches = 0;
        std::uint32_t maskedBatches = 0;
        std::uint32_t drawCalls = 0;
        std::uint32_t culled = 0;
        /// Fraction of the atlas's texels handed out, in [0, 1]. What says
        /// whether a dropped shadow was the cap's doing or the atlas's.
        float occupancy = 0.f;
    };

    /// @brief Clear the atlas and draw every request's faces into it.
    ///
    /// Requests are served in the order given, which is importance order, so a
    /// light that overflows is by construction one of the least important. That
    /// is what makes "overflow demotes the least important lights" true without a
    /// demotion pass: the atlas is fuller by the time they are reached, and each
    /// takes the largest class still available.
    ///
    /// A point light's six tiles are committed together or not at all — five
    /// faces of six is a light with a hole in it, which is a worse picture than
    /// the same light one class smaller.
    Stats Render(nvrhi::ICommandList *commandList, std::span<const LocalShadowRequest> requests,
                 std::span<const ShadowCaster> casters, const LocalShadowCasterIndex &casterIndex);

    [[nodiscard]] bool IsActive() const
    {
        return _active && _pipelines[static_cast<std::uint32_t>(MeshPipeline::Opaque)] != nullptr;
    }

    /// @brief The atlas the mesh shader samples. Never null after a successful
    /// Initialize() — the one-texel empty atlas while the pass is inactive.
    [[nodiscard]] nvrhi::ITexture *AtlasTexture() const { return _atlasTexture; }

    /// @brief Where each served light's views landed, valid until the next
    /// Render(). Parallel to nothing — a light the atlas could not serve has no
    /// entry at all, which is what makes a missing one mean "unshadowed".
    [[nodiscard]] std::span<const Tile> Tiles() const { return _tiles; }

    /// @brief The settings the current allocation was built for.
    [[nodiscard]] const LocalShadowSettings &Settings() const { return _settings; }

private:
    [[nodiscard]] bool RebuildTargets();
    [[nodiscard]] bool RebuildPipelines();
    [[nodiscard]] ShadowPipelines PipelineSet() const;
    void ReleaseTargets();
    /// @brief Create the one-texel atlas bound while the pass is inactive.
    [[nodiscard]] bool CreateNoAtlasTexture();

    /// @brief Cut tiles for every request the atlas can serve, filling @ref
    /// _tiles and @ref _targets. Returns how many requests went unserved.
    [[nodiscard]] std::uint32_t AllocateTiles(std::span<const LocalShadowRequest> requests);

    /// @brief Draw the views of @p tileRange's lights, whose views occupy
    /// @p viewRange of @ref _targets, in one draw-list build.
    ShadowDepthRenderer::Stats RenderChunk(nvrhi::ICommandList *commandList, std::uint32_t firstTile,
                                           std::uint32_t tileCount, std::uint32_t firstView,
                                           std::uint32_t viewCount, std::span<const ShadowCaster> casters,
                                           const LocalShadowCasterIndex &casterIndex);

    nvrhi::IDevice *_device = nullptr;
    const ShadowDepthRenderer *_depthRenderer = nullptr;

    std::array<nvrhi::GraphicsPipelineHandle, kMeshPipelineCount> _pipelines;

    nvrhi::TextureHandle _atlasTexture;
    // One framebuffer for the whole atlas: every tile is a rectangle of the same
    // texture and the viewport confines each draw, so there is nothing per-tile
    // for a framebuffer to say.
    nvrhi::FramebufferHandle _atlasFramebuffer;
    // Bound while the pass is inactive, so the mesh pass always has a texture
    // to sample. Permanent, not scaffolding: a scene with no shadowed lamp in
    // it never leaves it.
    nvrhi::TextureHandle _noAtlasTexture;

    ShadowAtlasAllocator _allocator;

    LocalShadowSettings _settings;
    bool _active = false;
    std::uint32_t _builtResolution = 0;
    ShadowMapFormat _builtFormat = ShadowMapFormat::D16;
    float _builtSlopeBias = -1.f;

    // Per-frame scratch, kept across frames so a steady state allocates nothing.
    std::vector<Tile> _tiles;
    std::vector<ShadowDepthTarget> _targets;
    // Where each tile's views start in _targets, so a chunk knows which views
    // belong to which light. _tiles.size() + 1 entries.
    std::vector<std::uint32_t> _tileViewStart;
    // The view mask each caster earns in the chunk being built, and the chunk
    // that last wrote it — a stamp rather than a clear, so a chunk costs the
    // casters it touches rather than the whole span.
    std::vector<std::uint32_t> _casterMask;
    std::vector<std::uint32_t> _casterStamp;
    std::uint32_t _chunkStamp = 0;
    std::vector<ShadowCaster> _chunkCasters;
};

} // namespace Assisi::Render
