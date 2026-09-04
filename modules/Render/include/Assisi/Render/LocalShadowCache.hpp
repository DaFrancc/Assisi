/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file LocalShadowCache.hpp
/// @brief Which atlas tiles still hold the right depth, and what has to be
/// redrawn because they do not.
///
/// A local light's tile is two layers. The still geometry's depth changes only
/// when that geometry does, so it is kept and copied; the moving geometry's is
/// redrawn every frame it is wanted. A light with nothing moving under it
/// therefore costs neither — its tile already holds what it should, from
/// whenever it was last drawn.
///
/// Everything that decides this is here, and it is here alone because this is
/// the one stage that can produce a **stale shadow**: a tile kept when it should
/// not have been is a wrong image with no visual tell until someone notices a
/// shadow that stopped following its object. Keeping the decision device-free is
/// what lets the missed-invalidation cases be written down as tests instead of
/// looked for on screen.
///
/// Two rules run through all of it:
///
///   * **Iterate what changed, never what exists.** Invalidation walks the
///     casters that moved against the lights that are lit — a handful against a
///     handful — and never the casters against the lights. A frame in which
///     nothing moved does no work at all, which is the whole claim.
///   * **When it is ambiguous, redraw.** Every intersection test here is
///     conservative in the direction that costs time rather than the one that
///     shows the wrong picture.
///
/// Nothing here allocates a tile or draws one. LocalShadowPass owns the atlas
/// and does both; this says what it should do.

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ShadowAtlas.hpp>
#include <Assisi/Render/ShadowImportance.hpp>
#include <Assisi/Render/ShadowSettings.hpp>

namespace Assisi::Render
{

/// @brief One caster, as the cache needs to see it: who it is and where it
/// stands.
///
/// The identity is opaque — the caller packs whatever is stable for it, and the
/// scene packs an entity. Deliberately not a pointer or an index into this
/// frame's caster span: those are re-derived every frame, and the cache has to
/// recognise a caster across the frames in which it was not gathered at all.
struct ShadowMover
{
    std::uint64_t casterId = 0;
    Geometry::BoundingSphere worldSphere;
};

/// @brief The most faces one local light spends, which is a point light's cube.
inline constexpr std::uint32_t kMaxLocalShadowFaces = kPointLightFaceCount;

/// @brief One light that won tiles this frame, with everything its views are
/// built from.
///
/// The geometry rather than the light component: nothing downstream has an
/// opinion about where a spot's aim came from, only that it points somewhere.
struct LocalShadowRequest
{
    LocalLightKind kind = LocalLightKind::Spot;
    /// Row in this kind's GPU light buffer, carried through so the caller can
    /// stamp the resulting view index back into the light the shader reads. It
    /// is also the identity the cache remembers this light by across frames.
    std::uint32_t lightIndex = 0;

    /// Where it stands and how far it reaches. Its own struct because the cache
    /// compares the whole of it against what a tile was baked at, and a field
    /// added here that the comparison did not know about would be a light that
    /// changed without its shadow noticing.
    LocalShadowLightPose pose;

    /// The tile size the selector asked for. A ceiling — the allocator demotes
    /// from here when the atlas cannot serve it.
    std::uint32_t sizeClass = 0;
};

/// @brief What one light's tile needs this frame.
///
/// One of these per request, in the request order — which is importance order,
/// so a plan further down the list is one for a light that matters less, and
/// that is what the budget spends itself against.
struct LocalShadowTilePlan
{
    /// Whether the light kept the rectangles it held last frame. False means the
    /// atlas must cut it new ones, and that whatever is in them belongs to
    /// somebody else.
    bool retained = false;

    /// The rectangles it kept, valid only while @ref retained. Faces beyond the
    /// light's own count are untouched.
    std::array<ShadowViewRect, kMaxLocalShadowFaces> rect{};

    /// Faces whose still-geometry layer no longer holds the right depth, as a
    /// bit each. Every bit of a light the atlas is cutting new tiles for, since
    /// a fresh rectangle holds another light's depth entirely.
    std::uint32_t dirtyFaces = 0;

    /// Whether anything is moving inside this light's reach. False is the
    /// resting case: the tile is composed of the cached layer and nothing else,
    /// so there is no draw to make at all.
    bool hasMovers = false;

    /// Whether the moving layer is redrawn this frame. False only under the
    /// update-rate throttle, and never for a light with no cached tile to fall
    /// back on.
    bool redrawMovers = true;

    /// Whether the budget refused this light's re-render.
    ///
    /// A refused light is **not served**: it holds no tile and lights
    /// unshadowed until a frame has room for it. That is the honest degrade —
    /// the alternative is a tile drawn from depth that no longer describes the
    /// scene, and a dimmer image is not the same kind of wrong as a false one.
    bool deferred = false;
};

/// @brief What one frame's planning came to, for the panel and the gates.
struct LocalShadowCachePlanStats
{
    /// Lights whose tile needed nothing drawn at all. The number the
    /// pay-for-what-you-place gate is read off: on a still scene it is every
    /// served light.
    std::uint32_t restingLights = 0;
    /// Faces whose still layer is being re-baked this frame.
    std::uint32_t bakedFaces = 0;
    /// Faces the budget refused, and so lights that went unshadowed for it.
    std::uint32_t deferredFaces = 0;
    std::uint32_t deferredLights = 0;
    /// Lights whose moving layer was skipped by the update-rate throttle.
    std::uint32_t throttledLights = 0;
    /// Casters drawn with the movers this frame, and casters folded back into
    /// the cached layer by having held still long enough.
    std::uint32_t dynamicCasters = 0;
    std::uint32_t promotedCasters = 0;
};

/// @brief Which casters are moving, and which have held still long enough to be
/// worth baking again.
///
/// The cached/dynamic split, and the hysteresis that keeps it from thrashing. A
/// caster's first moved frame **demotes** it: it leaves the cached layer of
/// every tile it touches, which costs one re-bake, and it draws with the movers
/// for as long as the motion lasts. After enough still frames it **promotes**
/// back, costing one more. So a motion episode costs two re-bakes however long
/// it runs, and standing still costs none.
///
/// That is the "amortized static" behaviour an authored mobility enum was once
/// thought to be needed for, out of tick data the engine already keeps.
class ShadowCasterMobility
{
public:
    ShadowCasterMobility() = default;

    /// @brief Record where a caster stood when it was baked into a tile.
    ///
    /// The pose the cached layer holds, which is what a later demotion has to
    /// invalidate — a caster leaving the cached layer must clear the tiles it
    /// was drawn into, not only the ones it has moved to. A caster never baked
    /// is in no cached layer, so an unrecorded one invalidates only where it now
    /// stands, which is exactly right.
    void NoteBaked(const ShadowMover &caster);

    /// @brief Fold this frame's movement in and age everything else.
    ///
    /// @p moved names the casters written this frame and where they now are.
    /// @p dynamicOut receives every caster that draws with the movers this frame.
    /// @p invalidateOut receives every caster that changed sides this frame,
    /// each with a sphere covering both the pose it is leaving and the pose it
    /// is taking — which is what the tiles are dirtied against.
    ///
    /// Both outputs are cleared and refilled.
    void Update(std::uint32_t frameIndex, std::uint32_t promoteStillFrames, std::span<const ShadowMover> moved,
                std::vector<ShadowMover> &dynamicOut, std::vector<ShadowMover> &invalidateOut);

    /// @brief Whether @p casterId draws with the movers rather than into the
    /// cached layer.
    [[nodiscard]] bool IsDynamic(std::uint64_t casterId) const;

    /// @brief How many casters are moving right now.
    [[nodiscard]] std::uint32_t DynamicCount() const { return _dynamicCount; }

    /// @brief Forget everything. What a level load wants, and what turning the
    /// cache off and on again wants: the recorded poses describe an atlas that
    /// no longer holds them.
    void Clear();

private:
    struct Record
    {
        /// Where the cached layer has this caster, if it is in one.
        Geometry::BoundingSphere bakedSphere;
        bool baked = false;

        /// Where it stands now, and the frame that was last true of.
        Geometry::BoundingSphere sphere;
        std::uint32_t lastMovedFrame = 0;
        bool dynamic = false;
    };

    std::unordered_map<std::uint64_t, Record> _casters;
    std::uint32_t _dynamicCount = 0;
};

/// @brief One frame, as the cache needs to see it.
///
/// The three spans are meaningless apart — the movers and the invalidations are
/// each walked against the requests and against nothing else — and the settings
/// and the frame counter decide what is done with the result.
struct LocalShadowCacheFrame
{
    /// Counts frames, and only has to advance by one per frame and never wrap
    /// during a session. Drives the update-rate throttle's phase and the tile
    /// ages the inspector reports.
    std::uint32_t frameIndex = 0;

    LocalShadowCacheSettings settings;

    /// The lights holding tiles, most important first. The order is what the
    /// update budget is spent down.
    std::span<const LocalShadowRequest> requests;

    /// Casters moving right now. They dirty nothing — they are in no cached
    /// layer — but a light one of them reaches has a moving layer to redraw.
    std::span<const ShadowMover> movers;

    /// Casters that changed sides this frame, each carrying a sphere covering
    /// both the pose it is leaving and the one it is taking. These are what
    /// invalidate a cached layer.
    std::span<const ShadowMover> invalidations;
};

/// @brief Which lights hold which tiles across frames, and what has gone stale
/// in them.
///
/// Lights are remembered by their buffer row, the same identity the selector's
/// hysteresis uses. Adding a light mid-scene shifts the rows below it and costs
/// those lights one re-bake — on a frame where the scene changed anyway.
class LocalShadowCache
{
public:
    LocalShadowCache() = default;

    /// @brief Decide what each of @p frame's requests needs this frame.
    ///
    /// The movers and the invalidations are walked against the requests and
    /// against nothing else, so this costs movers times lights — never casters
    /// times lights, and nothing at all on a frame in which nothing moved.
    ///
    /// @p out is filled with one plan per request, in the same order.
    void Plan(const LocalShadowCacheFrame &frame, std::vector<LocalShadowTilePlan> &out);

    /// @brief Record where the tiles actually landed and what was drawn into
    /// them.
    ///
    /// @p rects gives each served request its faces' rectangles, concatenated in
    /// request order with a light's faces consecutive; @p served says which
    /// requests got tiles at all and where each one's faces begin in @p rects. A
    /// request that went unserved — refused by the budget, or refused by the
    /// atlas — is evicted, because the rectangle it held is somebody else's now.
    void Commit(std::uint32_t frameIndex, std::span<const LocalShadowRequest> requests,
                std::span<const LocalShadowTilePlan> plans, std::span<const std::uint32_t> servedRequests,
                std::span<const ShadowViewRect> rects);

    /// @brief What the last Plan() decided, summed.
    [[nodiscard]] const LocalShadowCachePlanStats &Stats() const { return _stats; }

    /// @brief Every light holding a tile, for the atlas inspector.
    struct Residency
    {
        LocalLightKind kind = LocalLightKind::Spot;
        std::uint32_t lightIndex = 0;
        std::uint32_t faces = 0;
        std::uint32_t sizeClass = 0;
        std::array<ShadowViewRect, kMaxLocalShadowFaces> rect{};
        /// Frames since this light's still layer was last re-baked. Zero is a
        /// tile drawn this frame; a large number is a tile that has been paying
        /// nothing for a long time, which is what the cache is for — and, on a
        /// light something is visibly walking under, the shape a missed
        /// invalidation takes on screen.
        std::uint32_t ageFrames = 0;
    };

    /// @brief The residency table, valid until the next Commit().
    [[nodiscard]] std::span<const Residency> Tiles() const { return _report; }

    /// @brief Drop every tile. A settings change resizes or reformats the atlas,
    /// which makes every remembered rectangle a rectangle of a texture that no
    /// longer exists.
    void Forget();

private:
    struct Entry
    {
        std::uint32_t sizeClass = 0;
        std::uint32_t faces = 0;
        std::array<ShadowViewRect, kMaxLocalShadowFaces> rect{};
        LocalShadowLightPose pose;
        /// Faces whose still layer is out of date. Carried across frames: a face
        /// the budget refused stays dirty until a frame has room for it.
        std::uint32_t dirtyFaces = 0;
        std::uint32_t lastBakeFrame = 0;
        std::uint32_t lastMoverDrawFrame = 0;
        std::uint32_t lastSeenFrame = 0;
    };

    /// @brief The key a light is remembered by: its kind and its buffer row.
    [[nodiscard]] static std::uint64_t KeyOf(LocalLightKind kind, std::uint32_t lightIndex);

    std::unordered_map<std::uint64_t, Entry> _entries;
    std::vector<Residency> _report;
    LocalShadowCachePlanStats _stats;
};

/// @brief Which faces of a light @p sphere can cast into, as a bit each.
///
/// A spot light has one face and the answer is one bit or none. A point light
/// has six, and a caster near an axis casts into both faces that axis divides —
/// so this is an overlap test against each face's cone rather than a choice of
/// the nearest, and it errs wide: a sphere that only nearly reaches a face
/// dirties it anyway.
///
/// Zero means the caster is outside the light's reach entirely and can dirty
/// nothing.
[[nodiscard]] std::uint32_t LocalShadowFaceMask(const LocalShadowLightPose &pose, LocalLightKind kind,
                                                const Geometry::BoundingSphere &sphere);

} // namespace Assisi::Render
