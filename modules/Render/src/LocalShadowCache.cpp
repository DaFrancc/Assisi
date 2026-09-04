/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/LocalShadowCache.hpp>

#include <Assisi/Render/ShadowView.hpp>

#include <algorithm>
#include <cmath>

namespace Assisi::Render
{
namespace
{
/// @brief The cosine of the half-angle of the cone that contains one point-light
/// face's frustum.
///
/// A face is a square frustum, so the direction furthest from its axis is a
/// corner: `tan` of the half field of view along each axis, both at once, which
/// is that half angle scaled by the square's diagonal. Containing the frustum in
/// a cone is what makes the overlap test one dot product instead of four plane
/// tests, and it errs wide — the cone holds directions just outside the corners,
/// so a caster near one dirties the face rather than being missed by it.
float PointFaceConeCosine()
{
    const float halfFovRadians = glm::radians((90.f + kPointLightFaceOverlapDegrees) * 0.5f);
    const float diagonalTangent = std::tan(halfFovRadians) * std::sqrt(2.f);
    return 1.f / std::sqrt(1.f + diagonalTangent * diagonalTangent);
}

/// @brief A sphere containing both @p a and @p b.
///
/// What a caster leaving the cached layer has to be invalidated against: the
/// tiles that hold it at the pose it is abandoning as well as the ones it now
/// reaches. Two spheres one inside the other give the outer one back rather than
/// a larger sphere around both centres.
Geometry::BoundingSphere Merged(const Geometry::BoundingSphere &a, const Geometry::BoundingSphere &b)
{
    const glm::vec3 separation = b.center - a.center;
    const float distance = std::sqrt(glm::dot(separation, separation));
    if (distance + b.radius <= a.radius)
    {
        return a;
    }
    if (distance + a.radius <= b.radius)
    {
        return b;
    }
    const float radius = (distance + a.radius + b.radius) * 0.5f;
    // The centre sits on the line between them, at the midpoint of the two
    // furthest surface points. A zero distance is the concentric case, which the
    // containment tests above already answered.
    const glm::vec3 axis = distance > 0.f ? separation / distance : glm::vec3(0.f);
    return Geometry::BoundingSphere{a.center + axis * (radius - a.radius), radius};
}

/// @brief How many bits are set in @p mask.
std::uint32_t FaceCount(std::uint32_t mask)
{
    std::uint32_t count = 0;
    for (; mask != 0u; mask &= mask - 1u)
    {
        ++count;
    }
    return count;
}

/// @brief Every face of a light of @p kind, as a bit each.
std::uint32_t AllFaces(LocalLightKind kind)
{
    return (1u << LocalShadowFaceCount(kind)) - 1u;
}
} // namespace

std::uint32_t LocalShadowFaceMask(const LocalShadowLightPose &pose, LocalLightKind kind,
                                  const Geometry::BoundingSphere &sphere)
{
    const glm::vec3 separation = sphere.center - pose.position;
    const float reach = sphere.radius + pose.range;
    const float distanceSquared = glm::dot(separation, separation);
    if (!(distanceSquared <= reach * reach))
    {
        return 0; // outside the light's reach, so it occludes nothing for it
    }

    if (kind == LocalLightKind::Spot)
    {
        // One face, and its frustum test is the draw list's own. Refining here
        // would be a second answer to a question something downstream already
        // answers, and the two could disagree.
        return 1u;
    }

    const float distance = std::sqrt(distanceSquared);
    // A caster overlapping the light itself has no direction from it, and its
    // depth can land in any face. Both the divide below and the angular radius
    // stop meaning anything here, so every face is dirtied instead.
    if (!(distance > sphere.radius) || !(distance > 0.f))
    {
        return AllFaces(kind);
    }

    // The caster subtends a cone of half-angle asin(radius / distance) about the
    // direction to it, and a face covers a cone of its own. They overlap when
    // the angle between the axes is under the sum of the half-angles, which is
    // this comparison with the sum's cosine expanded — so the whole test is
    // arithmetic on sines and cosines already in hand.
    const glm::vec3 toCaster = separation / distance;
    const float sinCaster = sphere.radius / distance;
    const float cosCaster = std::sqrt(std::max(0.f, 1.f - sinCaster * sinCaster));

    static const float cosFace = PointFaceConeCosine();
    static const float sinFace = std::sqrt(std::max(0.f, 1.f - cosFace * cosFace));

    const float cosSum = cosFace * cosCaster - sinFace * sinCaster;
    // The two cones together span more than a hemisphere from the light, so
    // there is no direction they exclude.
    if (cosSum <= -1.f)
    {
        return AllFaces(kind);
    }

    std::uint32_t mask = 0;
    for (std::uint32_t face = 0; face < kPointLightFaceCount; ++face)
    {
        if (glm::dot(toCaster, PointLightFaceDirection(face)) >= cosSum)
        {
            mask |= 1u << face;
        }
    }
    // A caster inside the light's reach casts into something. An empty mask
    // would be the conservative rule failing silently, so it degrades to every
    // face rather than to none.
    return mask == 0u ? AllFaces(kind) : mask;
}

void ShadowCasterMobility::NoteBaked(const ShadowMover &caster)
{
    Record &record = _casters[caster.casterId];
    record.bakedSphere = caster.worldSphere;
    record.baked = true;
    if (!record.dynamic)
    {
        record.sphere = caster.worldSphere;
    }
}

void ShadowCasterMobility::Update(std::uint32_t frameIndex, std::uint32_t promoteStillFrames,
                                  std::span<const ShadowMover> moved, std::vector<ShadowMover> &dynamicOut,
                                  std::vector<ShadowMover> &invalidateOut)
{
    dynamicOut.clear();
    invalidateOut.clear();
    promoteStillFrames = std::max(promoteStillFrames, 1u);

    for (const ShadowMover &mover : moved)
    {
        Record &record = _casters[mover.casterId];
        if (!record.dynamic)
        {
            // The demotion. The cached layer of every tile holding it must lose
            // it, which is the pose it was baked at, and it must appear at the
            // pose it has moved to — so both are invalidated. A caster never
            // baked is in no cached layer, and only where it now stands matters.
            record.dynamic = true;
            ++_dynamicCount;
            const Geometry::BoundingSphere leaving = record.baked ? record.bakedSphere : mover.worldSphere;
            invalidateOut.push_back(ShadowMover{mover.casterId, Merged(leaving, mover.worldSphere)});
        }
        record.sphere = mover.worldSphere;
        record.lastMovedFrame = frameIndex;
    }

    for (auto entry = _casters.begin(); entry != _casters.end();)
    {
        Record &record = entry->second;
        if (!record.dynamic)
        {
            ++entry;
            continue;
        }
        if (frameIndex - record.lastMovedFrame < promoteStillFrames)
        {
            dynamicOut.push_back(ShadowMover{entry->first, record.sphere});
            ++entry;
            continue;
        }

        // The promotion. It rejoins the cached layer, which every tile it
        // reaches has to be told about — the layer was baked without it.
        record.dynamic = false;
        --_dynamicCount;
        invalidateOut.push_back(ShadowMover{entry->first, record.sphere});
        // Dropped rather than kept still: a caster that has settled is
        // indistinguishable from one that never moved, and keeping a record per
        // caster that has ever moved would grow without bound over a level's
        // life. Its baked pose is re-recorded by the bake it just triggered.
        entry = _casters.erase(entry);
    }
}

bool ShadowCasterMobility::IsDynamic(std::uint64_t casterId) const
{
    const auto found = _casters.find(casterId);
    return found != _casters.end() && found->second.dynamic;
}

void ShadowCasterMobility::Clear()
{
    _casters.clear();
    _dynamicCount = 0;
}

std::uint64_t LocalShadowCache::KeyOf(LocalLightKind kind, std::uint32_t lightIndex)
{
    return (static_cast<std::uint64_t>(kind) << 32) | lightIndex;
}

void LocalShadowCache::Forget()
{
    _entries.clear();
    _report.clear();
    _stats = LocalShadowCachePlanStats{};
}

void LocalShadowCache::Plan(const LocalShadowCacheFrame &frame, std::vector<LocalShadowTilePlan> &out)
{
    const std::span<const LocalShadowRequest> requests = frame.requests;

    _stats = LocalShadowCachePlanStats{};
    _stats.dynamicCasters = static_cast<std::uint32_t>(frame.movers.size());

    out.assign(requests.size(), LocalShadowTilePlan{});

    // Residency. A light keeps its rectangles only if it is asking for the same
    // tile size and standing where the depth in them was recorded from.
    // Anything else is a light the atlas must cut new tiles for, and a new
    // rectangle holds whatever the light that had it left behind.
    for (std::size_t index = 0; index < requests.size(); ++index)
    {
        const LocalShadowRequest &request = requests[index];
        LocalShadowTilePlan &plan = out[index];
        const std::uint32_t faces = LocalShadowFaceCount(request.kind);

        const auto found = _entries.find(KeyOf(request.kind, request.lightIndex));
        if (found == _entries.end() || found->second.sizeClass != request.sizeClass || found->second.faces != faces ||
            !(found->second.pose == request.pose))
        {
            plan.dirtyFaces = AllFaces(request.kind);
            continue;
        }

        plan.retained = true;
        plan.rect = found->second.rect;
        // Carried, not recomputed: a face the budget refused last frame is still
        // out of date this frame, and forgetting it is exactly the missed
        // invalidation this whole file exists to prevent.
        plan.dirtyFaces = found->second.dirtyFaces;
    }

    // Invalidation, and the only place a caster meets a light. Movers times
    // lights — never casters times lights — because the set walked here is what
    // changed rather than what exists, and on a still frame it is empty.
    const auto dirty = [&](std::span<const ShadowMover> casters, bool countsAsMotion)
                       {
                           for (const ShadowMover &caster : casters)
                           {
                               for (std::size_t index = 0; index < requests.size(); ++index)
                               {
                                   const std::uint32_t mask =
                                       LocalShadowFaceMask(requests[index].pose, requests[index].kind, caster.worldSphere);
                                   if (mask == 0u)
                                   {
                                       continue;
                                   }
                                   if (countsAsMotion)
                                   {
                                       out[index].hasMovers = true;
                                   }
                                   else
                                   {
                                       out[index].dirtyFaces |= mask;
                                   }
                               }
                           }
                       };
    // A caster that is merely moving does not invalidate anything: it is not in
    // the cached layer at all, and its depth is redrawn over the copy every
    // frame. Only changing sides invalidates, which is what makes a motion
    // episode two re-bakes rather than one per frame.
    dirty(frame.movers, true);
    dirty(frame.invalidations, false);

    // The throttle. A light near the top of the ordering always redraws; the
    // divisor is spent further down, where a frame of lag is not what anyone is
    // looking at. Phased by the light's own row so two throttled lights do not
    // land on the same frame.
    const std::uint32_t divisor = std::max(frame.settings.movingLightUpdateDivisor, 1u);
    const auto atFullRate = static_cast<std::size_t>(requests.size() / (divisor == 0u ? 1u : divisor));
    for (std::size_t index = 0; index < requests.size(); ++index)
    {
        LocalShadowTilePlan &plan = out[index];
        if (!plan.hasMovers || !plan.retained || divisor <= 1u || index < atFullRate)
        {
            continue;
        }
        const std::uint32_t phase = (frame.frameIndex + requests[index].lightIndex) % divisor;
        if (phase != 0u)
        {
            plan.redrawMovers = false;
            ++_stats.throttledLights;
        }
    }

    // The budget, spent in request order — which is importance order, so what it
    // runs out on is by construction the least important light asking. A light
    // it refuses holds no tile and lights unshadowed, rather than lighting from
    // a tile whose depth no longer describes the scene.
    std::uint32_t remaining = std::max(frame.settings.updateBudgetFaces, 1u);
    for (std::size_t index = 0; index < requests.size(); ++index)
    {
        LocalShadowTilePlan &plan = out[index];
        const std::uint32_t wanted = FaceCount(plan.dirtyFaces);
        if (wanted == 0u)
        {
            if (!plan.hasMovers)
            {
                ++_stats.restingLights;
            }
            continue;
        }
        if (wanted > remaining)
        {
            plan.deferred = true;
            _stats.deferredFaces += wanted;
            ++_stats.deferredLights;
            continue;
        }
        remaining -= wanted;
        _stats.bakedFaces += wanted;
    }
}

void LocalShadowCache::Commit(std::uint32_t frameIndex, std::span<const LocalShadowRequest> requests,
                              std::span<const LocalShadowTilePlan> plans, std::span<const std::uint32_t> servedRequests,
                              std::span<const ShadowViewRect> rects)
{
    _report.clear();

    std::size_t rectCursor = 0;
    for (const std::uint32_t index : servedRequests)
    {
        if (index >= requests.size() || index >= plans.size())
        {
            continue;
        }
        const LocalShadowRequest &request = requests[index];
        const LocalShadowTilePlan &plan = plans[index];
        const std::uint32_t faces = LocalShadowFaceCount(request.kind);
        if (rectCursor + faces > rects.size())
        {
            break; // a caller whose rect span does not match what it served
        }

        Entry &entry = _entries[KeyOf(request.kind, request.lightIndex)];
        entry.sizeClass = request.sizeClass;
        entry.faces = faces;
        for (std::uint32_t face = 0; face < faces; ++face)
        {
            entry.rect[face] = rects[rectCursor + face];
        }
        entry.pose = request.pose;
        // A served light is one whose dirty faces were drawn: the budget only
        // ever serves a light it can bake whole, so what is committed is clean.
        entry.dirtyFaces = 0;
        if (plan.dirtyFaces != 0u)
        {
            entry.lastBakeFrame = frameIndex;
        }
        if (plan.hasMovers && plan.redrawMovers)
        {
            entry.lastMoverDrawFrame = frameIndex;
        }
        entry.lastSeenFrame = frameIndex;
        rectCursor += faces;

        Residency &row = _report.emplace_back();
        row.kind = request.kind;
        row.lightIndex = request.lightIndex;
        row.faces = faces;
        row.sizeClass = request.sizeClass;
        row.rect = entry.rect;
        row.ageFrames = frameIndex - entry.lastBakeFrame;
    }

    // A light not served this frame has no tile, and the rectangles it held are
    // free for somebody else — so remembering them would hand it back depth
    // another light has since written. Dropped rather than kept dirty.
    for (auto entry = _entries.begin(); entry != _entries.end();)
    {
        entry = entry->second.lastSeenFrame == frameIndex ? std::next(entry) : _entries.erase(entry);
    }
}

} // namespace Assisi::Render
