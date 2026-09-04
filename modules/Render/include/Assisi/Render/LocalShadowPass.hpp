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
#include <Assisi/Render/LocalShadowCache.hpp>
#include <Assisi/Render/ShadowAtlas.hpp>
#include <Assisi/Render/ShadowDepthRenderer.hpp>
#include <Assisi/Render/ShadowImportance.hpp>
#include <Assisi/Render/ShadowSettings.hpp>
#include <Assisi/Render/ShadowView.hpp>

namespace Assisi::Render
{

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
        std::uint32_t views = 0;  ///< Atlas faces the frame's view table describes.
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

        /// Faces whose still layer was re-rendered, and tile copies made to
        /// compose a face from it. Both are zero on a resting frame, which is
        /// the pay-for-what-you-place gate read per light rather than per scene.
        std::uint32_t bakedFaces = 0;
        std::uint32_t copiedFaces = 0;
        /// Lights whose tile was served straight out of the cache, with nothing
        /// drawn and nothing copied.
        std::uint32_t restingLights = 0;
        /// Lights the update budget refused, and so lights that went unshadowed
        /// this frame rather than shadowed from a tile out of date.
        std::uint32_t deferredLights = 0;
        /// Casters drawn over the cached layer because they are moving.
        std::uint32_t dynamicCasters = 0;
    };

    /// @brief Everything one Render() needs about the frame.
    ///
    /// A struct because the caching half added two spans that are meaningless
    /// apart from each other, and a call taking six spans reads as an ordering
    /// puzzle at every call site.
    struct Frame
    {
        /// The lights that won tiles, most important first. The order is load
        /// bearing twice over: the atlas fills from the top, so a light that
        /// overflows is by construction one of the least important, and the
        /// update budget is spent down the same list.
        std::span<const LocalShadowRequest> requests;

        /// Every caster reaching any of those lights, sorted, each carrying
        /// whether it is moving. Empty is legitimate and is what a frame with
        /// nothing to redraw passes — see @ref casterIndex.
        std::span<const ShadowCaster> casters;

        /// Which casters reach which request. Must describe exactly @ref
        /// requests, even when the caster span is empty: a still frame skips
        /// the gather and passes empty rows, which is the whole saving.
        const LocalShadowCasterIndex *casterIndex = nullptr;

        /// Casters that are moving right now. They dirty nothing — they are not
        /// in any cached layer — but a light one of them reaches has a moving
        /// layer to redraw.
        std::span<const ShadowMover> movers;

        /// Casters that changed sides this frame, each with a sphere covering
        /// both the pose it is leaving and the one it is taking. These are what
        /// invalidate a cached layer.
        std::span<const ShadowMover> invalidations;

        /// Counts frames, and only has to advance by one per frame and never
        /// wrap during a session. Drives the update-rate throttle's phase and
        /// the tile ages the inspector reports.
        std::uint32_t frameIndex = 0;
    };

    /// @brief Decide what this frame needs, before its casters are gathered.
    ///
    /// The gather is the expensive half — it walks every mesh in the scene and
    /// resolves its bounds and its material — and a frame that draws nothing
    /// must not pay it. But whether a frame draws nothing is a question only the
    /// cache can answer: a caster that moved is not the only thing that dirties
    /// a tile, and a light that moved, or came back from not casting, or lost
    /// its tile to somebody else needs its still layer baked from casters that
    /// have not moved at all.
    ///
    /// So the caller asks first and gathers only if told to. @p frame's caster
    /// span and index are unread here and may be empty.
    ///
    /// @return whether anything must be drawn this frame. False is the resting
    /// case: every served tile already holds what it should.
    [[nodiscard]] bool PlanFrame(const Frame &frame);

    /// @brief Bring every served light's tile up to date, drawing as little as
    /// the frame allows.
    ///
    /// Uses the decision PlanFrame made for the same @ref Frame::frameIndex, and
    /// makes it itself if there is none — so a caller that does not care about
    /// the gather can call this alone and still be correct.
    ///
    /// A point light's six tiles are committed together or not at all — five
    /// faces of six is a light with a hole in it, which is a worse picture than
    /// the same light one class smaller.
    ///
    /// With caching off this clears the whole atlas and redraws every face of
    /// every served light, which is the baseline the cached path is measured
    /// against and reproduces exactly.
    Stats Render(nvrhi::ICommandList *commandList, const Frame &frame);

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

    /// @brief Which light holds which tile and how long its still layer has
    /// stood, for the atlas inspector. Empty while caching is off, where the
    /// question does not arise: every tile is redrawn every frame.
    [[nodiscard]] std::span<const LocalShadowCache::Residency> CachedTiles() const { return _cache.Tiles(); }

private:
    [[nodiscard]] bool RebuildTargets();
    [[nodiscard]] bool RebuildPipelines();
    [[nodiscard]] ShadowPipelines PipelineSet() const;
    void ReleaseTargets();
    /// @brief Create the one-texel atlas bound while the pass is inactive.
    [[nodiscard]] bool CreateNoAtlasTexture();
    /// @brief Create or drop the kept-depth atlas and the cleared tile that
    /// blanks a rectangle of it, to match @ref LocalShadowCacheSettings::enabled.
    [[nodiscard]] bool RebuildCacheTargets();

    /// @brief Cut tiles for every request the atlas can serve, filling @ref
    /// _tiles, @ref _targets and @ref _servedRequests. Returns how many requests
    /// went unserved.
    ///
    /// Two passes, and the order between them is the point: every tile a light
    /// is keeping is reserved before any fresh one is cut, because a fresh cut
    /// takes whichever node is free and that node may be one a later reservation
    /// wanted.
    [[nodiscard]] std::uint32_t AllocateTiles(std::span<const LocalShadowRequest> requests);

    /// @brief Which side of the cached/dynamic split a chunk draws.
    enum class CasterSide : std::uint8_t
    {
        /// The still geometry, drawn into the kept layer when it is re-baked.
        Static,
        /// The moving geometry, drawn over a copy of that layer every frame.
        Dynamic,
        /// Everything, which is the uncached path.
        Both,
    };

    /// @brief A run of views to draw, and the request each one takes its casters
    /// from.
    ///
    /// The two spans are parallel and meaningless apart: a view with no request
    /// has no row to cull against, and a request with no view has nothing to
    /// draw into.
    struct TargetRun
    {
        std::span<const ShadowDepthTarget> targets;
        std::span<const std::uint32_t> request;
    };

    /// @brief Draw @p run, chunked to fit a caster's view mask.
    ///
    /// @p side filters the casters a target takes, which is what separates the
    /// bake from the composite: they walk the same rows and keep opposite halves.
    ShadowDepthRenderer::Stats RenderTargets(nvrhi::ICommandList *commandList, const TargetRun &run, CasterSide side,
                                             const Frame &frame);

    /// @brief Blank the rectangle @p rect of the kept-depth atlas, by copying a
    /// cleared tile over it.
    ///
    /// A depth clear is a whole-attachment operation, and this needs one
    /// rectangle of a shared texture — clearing the attachment would wipe every
    /// other light's kept depth to re-bake one. A copy from a tile cleared once
    /// at creation is the same result confined to the rectangle, and it is the
    /// same kind of transfer the composite already makes.
    void BlankCacheTile(nvrhi::ICommandList *commandList, const ShadowViewRect &rect);

    /// @brief Copy the kept layer of @p rect over the live atlas, which is the
    /// composite's first half.
    void CopyCachedTile(nvrhi::ICommandList *commandList, const ShadowViewRect &rect);

    /// @brief Redraw every served face into the live atlas from nothing, which
    /// is what the pass did before there was a cache.
    Stats RenderUncached(nvrhi::ICommandList *commandList, const Frame &frame, Stats stats);

    /// @brief Re-bake the stale layers, compose the tiles that need it, and
    /// leave the resting ones alone.
    Stats RenderCached(nvrhi::ICommandList *commandList, const Frame &frame, Stats stats);

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

    // The still geometry's depth, at the same rectangles as the live atlas. Its
    // own texture rather than a second half of one, so a tile's kept depth and
    // the tile composed from it are the same rectangle of two textures and the
    // copy between them needs no offset.
    nvrhi::TextureHandle _cacheTexture;
    nvrhi::FramebufferHandle _cacheFramebuffer;
    // One tile's worth of depth at 1.0, cleared once and copied wherever a
    // rectangle has to be blanked. Sized for the largest tile the settings can
    // hand out.
    nvrhi::TextureHandle _clearTile;
    std::uint32_t _builtClearTileSize = 0;
    bool _builtCacheEnabled = false;
    // Whether the clear tile has been filled with far depth yet. It needs a
    // command list, which creation does not have, so the first frame that has to
    // blank anything fills it.
    bool _clearTileReady = false;

    ShadowAtlasAllocator _allocator;
    LocalShadowCache _cache;

    LocalShadowSettings _settings;
    bool _active = false;
    std::uint32_t _builtResolution = 0;
    ShadowMapFormat _builtFormat = ShadowMapFormat::D16;
    float _builtSlopeBias = -1.f;

    // Per-frame scratch, kept across frames so a steady state allocates nothing.
    std::vector<Tile> _tiles;
    std::vector<ShadowDepthTarget> _targets;
    // Which request each entry of _targets came from, so a target can find its
    // casters without the tile it belongs to being reconstructed.
    std::vector<std::uint32_t> _targetRequest;
    // Which face of its light each target is, for matching against a plan's
    // dirty-face bits.
    std::vector<std::uint32_t> _targetFace;
    // Where each tile's views start in _targets. _tiles.size() + 1 entries.
    std::vector<std::uint32_t> _tileViewStart;
    // The request each served tile came from, and every served face's rectangle
    // concatenated in that order — what the cache is told to remember.
    std::vector<std::uint32_t> _servedRequests;
    std::vector<ShadowViewRect> _servedRects;
    // This frame's plan, one per request, and the frame PlanFrame made it for.
    // Render remakes it when they disagree, so a caller that never calls
    // PlanFrame is merely slower rather than wrong.
    std::vector<LocalShadowTilePlan> _plans;
    std::uint32_t _plannedFrame = 0;
    bool _planned = false;
    // The subset of _targets whose kept layer is being re-baked, and the request
    // each came from.
    std::vector<ShadowDepthTarget> _bakeTargets;
    std::vector<std::uint32_t> _bakeTargetRequest;
    // The view mask each caster earns in the chunk being built, and the chunk
    // that last wrote it — a stamp rather than a clear, so a chunk costs the
    // casters it touches rather than the whole span.
    std::vector<std::uint32_t> _casterMask;
    std::vector<std::uint32_t> _casterStamp;
    std::uint32_t _chunkStamp = 0;
    std::vector<ShadowCaster> _chunkCasters;
};

} // namespace Assisi::Render
