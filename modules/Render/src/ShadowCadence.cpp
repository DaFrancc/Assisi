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

/// Whether two fits cover the same stretch of view distance, exactly.
///
/// Exactly, never within a tolerance: the splits are recomputed from unchanged
/// inputs by the same arithmetic every frame, so any difference at all is a
/// changed field of view, clip range or setting the kept map was not fitted for.
bool SameSplits(const ShadowCascade &lhs, const ShadowCascade &rhs)
{
    return lhs.splitNearView == rhs.splitNearView && lhs.splitFarView == rhs.splitFarView;
}

/// Whether @p candidate's sphere lies inside the box @p kept's map covers.
///
/// Tested in the kept map's own clip space, where its box is [-1, 1] across and
/// [0, 1] along the light. The projection is orthographic, so a sphere maps to
/// an axis-aligned box of the radius over the extent across, and the radius over
/// the depth range along.
bool Contains(const ShadowCascade &kept, const ShadowCascade &candidate)
{
    if (!(kept.extent > 0.f) || candidate.radius > kept.extent)
    {
        return false;
    }
    const glm::vec4 clip = kept.viewProjection * glm::vec4(candidate.center, 1.f);
    const float across = candidate.radius / kept.extent;
    const float along = candidate.radius / kept.depthRange;
    return std::abs(clip.x) + across <= 1.f && std::abs(clip.y) + across <= 1.f && clip.z - along >= 0.f &&
           clip.z + along <= 1.f;
}
} // namespace

bool SunShadowCadence::WithinTolerance(const Slice &slice, const ShadowCascade &candidate,
                                       const glm::vec3 &lightDirection, float driftTexels)
{
    if (!SameSplits(slice.cascade, candidate))
    {
        return false;
    }

    // The map covers the slice with a margin (see kCascadePadding), so it stays
    // right for as long as the slice the camera now sees is inside it. The
    // candidate's own snapped centre does not matter: the kept map is sampled
    // through its own matrix, on its own texel lattice.
    if (!Contains(slice.cascade, candidate))
    {
        return false;
    }

    // Measured against the texel of the depth actually held rather than the
    // candidate's: the bound is a claim about how far the picture in this slice
    // has slid, and this slice is the one with the texels.
    const float allowance = driftTexels * slice.cascade.worldUnitsPerTexel;

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
    _movingSignature = {};
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

    // A caster changing sides is the only motion a still layer answers to. Its
    // sphere already covers both the pose the layer holds it at and the one it
    // takes, so a caster leaving the layer is erased from where it was, not
    // only drawn where it went.
    for (const ShadowMover &changed : frame.invalidations)
    {
        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (!_slices[i].valid || (dirty & (1u << i)) != 0u)
            {
                continue;
            }
            if (CasterReachesShadowedVolume(changed.worldSphere, volumes[i], _slices[i].lightDirection))
            {
                dirty |= 1u << i;
                ++_stats.dirtiedByMotion;
            }
        }
    }

    for (std::uint32_t i = 0; i < count; ++i)
    {
        if ((dirty & (1u << i)) != 0u)
        {
            _slices[i] = Slice{.cascade = candidate.cascades[i],
                               .lightDirection = lightDirection,
                               .drawFrame = frame.frameIndex,
                               .valid = true};
            // The sampled slice is copied whole from the new still depth, which
            // carries no movers until they are drawn over it again.
            _movingSignature[i] = 0;
            volumes[i] = CascadeVolumeBounds(_slices[i].cascade);
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

    // The movers. Walked as movers times cascades, like the rest of this, and
    // on a frame they stand where they stood nothing is drawn.
    std::array<std::uint64_t, kMaxShadowCascades> signature{};
    for (const ShadowMover &mover : frame.dynamic)
    {
        const std::uint64_t contribution = ShadowMoverSignature(mover);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (CasterReachesShadowedVolume(mover.worldSphere, volumes[i], _slices[i].lightDirection))
            {
                signature[i] += contribution;
            }
        }
    }
    for (std::uint32_t i = 0; i < count; ++i)
    {
        // Zero to zero is a cascade nothing moves in and nothing did; anything
        // else changed what is drawn over the still depth — including every
        // mover leaving, whose texels still have to be put back.
        if (signature[i] != _movingSignature[i])
        {
            _movingSignature[i] = signature[i];
            out.movingRedraw[out.movingRedrawCount++] = i;
            ++_stats.movingRedrawn;
        }
        else if (signature[i] != 0u)
        {
            ++_stats.movingKept;
        }
    }
}

} // namespace Assisi::Render
