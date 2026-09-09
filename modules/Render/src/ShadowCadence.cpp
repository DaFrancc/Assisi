/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ShadowCadence.hpp>

#include <algorithm>
#include <cmath>

namespace Assisi::Render
{
namespace
{
/// The light direction, normalised, with a degenerate one answered by straight
/// down rather than by a NaN that would take the whole comparison with it.
glm::vec3 SafeDirection(const glm::vec3 &direction)
{
    const float lengthSquared = glm::dot(direction, direction);
    if (!(lengthSquared > 0.f) || !std::isfinite(lengthSquared))
    {
        return glm::vec3{0.f, -1.f, 0.f};
    }
    return direction * (1.f / std::sqrt(lengthSquared));
}

/// Whether two fits describe the same box, exactly.
///
/// Exactly, never within a tolerance: the splits and the radius are recomputed
/// from unchanged inputs by the same arithmetic every frame, so a still camera
/// produces bit-identical values and any difference at all is one the depth in
/// the slice did not see. Everything else a fit carries — the texel size, the
/// depth range, the matrix — is derived from the radius, so comparing the radius
/// covers them.
bool SameShape(const ShadowCascade &lhs, const ShadowCascade &rhs)
{
    return lhs.splitNearView == rhs.splitNearView && lhs.splitFarView == rhs.splitFarView && lhs.radius == rhs.radius;
}
} // namespace

bool SunShadowCadence::WithinTolerance(const Slice &slice, const ShadowCascade &candidate,
                                       const glm::vec3 &lightDirection, float driftTexels)
{
    if (!SameShape(slice.cascade, candidate))
    {
        return false;
    }

    // Measured against the texel of the depth actually held rather than the
    // candidate's: the bound is a claim about how far the picture in this slice
    // has slid, and this slice is the one with the texels.
    const float allowance = driftTexels * slice.cascade.worldUnitsPerTexel;

    // Where the fitted centre has walked to. The centre is snapped to the
    // cascade's own texel lattice, so it moves in whole texels or not at all —
    // which is why a tolerance under one texel means the camera may not cross a
    // texel boundary, and why free-look gameplay trips this nearly every frame.
    //
    // This is also the term that gives the cascades different cadences: a near
    // cascade's texel is centimetres and a far one's is metres, so walking
    // crosses the first constantly and the last hardly at all.
    const glm::vec3 displacement = candidate.center - slice.cascade.center;
    if (glm::dot(displacement, displacement) > allowance * allowance)
    {
        return false;
    }

    // The sun's rotation times the lever arm is how far a shadow edge moves: an
    // occluder at the far end of the cascade's depth range swings by the angle
    // times that range.
    //
    // It trips every cascade at once, and that is arithmetic rather than an
    // oversight. A cascade's depth range is twice its radius and its texel is
    // twice its radius over the resolution, so the ratio between them is the
    // resolution in every cascade alike — the same identity that lets one bias
    // setting hold across all of them. Only the centre term above separates the
    // cascades' cadences.
    const float rotation = Math::AngleBetween(slice.lightDirection, lightDirection);
    return rotation * slice.cascade.depthRange <= allowance;
}

void SunShadowCadence::Forget()
{
    _slices = {};
    _drawnPose.clear();
    _stats = SunShadowCadenceStats{};
}

void SunShadowCadence::Plan(const SunShadowCadenceFrame &frame, const CascadeFit &candidate, SunShadowCadencePlan &out)
{
    _stats = SunShadowCadenceStats{};
    out = SunShadowCadencePlan{};

    const std::uint32_t count = std::min<std::uint32_t>(candidate.count, kMaxShadowCascades);
    if (count == 0)
    {
        // Nothing is fitted, so nothing holds depth worth keeping. Dropping the
        // slices here is what stops a cascade fitted before the sun went out
        // from being kept across the frames it was gone for.
        Forget();
        return;
    }
    out.fit.count = count;

    const glm::vec3 lightDirection = SafeDirection(frame.lightDirection);
    const std::uint32_t everyCascade = count == 32u ? ~0u : (1u << count) - 1u;

    // Which cascades this frame must draw, as a mask so one dirtied twice is
    // still drawn once. First pass: what the fit and the sun have done.
    std::uint32_t dirty = 0;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const Slice &slice = _slices[i];
        if (!frame.settings.enabled || !slice.valid ||
            !WithinTolerance(slice, candidate.cascades[i], lightDirection, frame.settings.driftTexels))
        {
            dirty |= 1u << i;
        }
    }

    // The volume a caster has to reach to matter to each slice — the slice's
    // own, not the candidate's, because a slice being kept is a slice whose
    // depth is what the caster would be missing from.
    std::array<Geometry::BoundingSphere, kMaxShadowCascades> volumes{};
    for (std::uint32_t i = 0; i < count; ++i)
    {
        volumes[i] = CascadeVolumeBounds(_slices[i].cascade);
    }

    for (const ShadowMover &mover : frame.movers)
    {
        const auto recorded = _drawnPose.find(mover.casterId);
        if (recorded == _drawnPose.end())
        {
            // Nothing here knows which slices were drawn holding this caster, so
            // every one of them might have been. Conservative in the direction
            // that costs a frame rather than the one that leaves a shadow
            // standing where its object used to be.
            ++_stats.unrecordedMovers;
            dirty = everyCascade;
            _drawnPose.emplace(mover.casterId, Record{mover.worldSphere, frame.frameIndex});
            continue;
        }

        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (!_slices[i].valid || (dirty & (1u << i)) != 0u)
            {
                continue;
            }
            // Both poses: the one the depth holds it at, which the slice has to
            // lose, and the one it has moved to, which the slice has to gain.
            // Testing only where it now stands is the missed invalidation that
            // leaves a shadow behind a caster that walked out of the cascade.
            if (CasterReachesShadowedVolume(recorded->second.sphere, volumes[i], _slices[i].lightDirection) ||
                CasterReachesShadowedVolume(mover.worldSphere, volumes[i], _slices[i].lightDirection))
            {
                dirty |= 1u << i;
                ++_stats.dirtiedByMotion;
            }
        }
        recorded->second = Record{mover.worldSphere, frame.frameIndex};
    }

    for (std::uint32_t i = 0; i < count; ++i)
    {
        if ((dirty & (1u << i)) != 0u)
        {
            _slices[i] = Slice{.cascade = candidate.cascades[i],
                               .lightDirection = lightDirection,
                               .drawFrame = frame.frameIndex,
                               .valid = true};
            out.redraw[out.redrawCount++] = i;
            ++_stats.redrawn;
        }
        else
        {
            ++_stats.kept;
        }
        out.fit.cascades[i] = _slices[i].cascade;
        out.ageFrames[i] = frame.frameIndex - _slices[i].drawFrame;
    }

    // A pose no cascade holds, on a caster that is not moving right now, is a
    // pose nothing can be invalidated against. Dropped, because a record per
    // caster that has ever moved would grow without bound over a level's life.
    //
    // A caster still moving keeps its record however far outside the cascades it
    // is, which is what stops something wandering past the shadow distance from
    // dirtying every cascade on every frame of its journey.
    for (auto entry = _drawnPose.begin(); entry != _drawnPose.end();)
    {
        bool held = entry->second.moveFrame == frame.frameIndex;
        for (std::uint32_t i = 0; i < count && !held; ++i)
        {
            held = _slices[i].valid && CasterReachesShadowedVolume(entry->second.sphere,
                                                                   CascadeVolumeBounds(_slices[i].cascade),
                                                                   _slices[i].lightDirection);
        }
        entry = held ? std::next(entry) : _drawnPose.erase(entry);
    }
}

} // namespace Assisi::Render
