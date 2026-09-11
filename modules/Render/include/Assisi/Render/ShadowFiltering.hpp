/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShadowFiltering.hpp
/// @brief The kernels both shadow lookups filter with, as arithmetic the CPU can
/// check: the tent, the contact-hardening search and its disk, and the bias that
/// covers a comparison's footprint.
///
/// mesh.frag carries each of these, and the two must agree — this side exists so
/// the claims the shader rests on can be tested without a GPU.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ShadowSettings.hpp>

#include <array>
#include <cstdint>

namespace Assisi::Render
{

/// @brief The half-width, in texels, of the tent a grid filter of @p radiusTaps
/// becomes: its reach plus the half texel the grid's outermost tap blended over.
[[nodiscard]] constexpr float TentHalfWidthTexels(float radiusTaps)
{
    return radiusTaps + 0.5f;
}

/// @brief The tent contact hardening never filters narrower than: three texels
/// wide, the smallest kernel that hides the map's grid.
inline constexpr float kPcssFloorHalfWidthTexels = TentHalfWidthTexels(kPcf3FilterRadiusTaps);

/// @brief The most bilinear taps a tent takes along one axis. A tent of
/// half-width h covers 2h + 1 texels at worst, and each tap reads two of them.
inline constexpr std::uint32_t kMaxTentAxisTaps = 3;

/// @brief The contact-hardening disk's tap counts: the fewest, where it first
/// widens past its tent floor, and the most, where its penumbra is widest.
inline constexpr std::uint32_t kPcssMinTaps = 16;
inline constexpr std::uint32_t kPcssMaxTaps = 64;

/// @brief Taps a disk reads before deciding it straddles an edge at all. Every
/// tap count is a multiple of this, so the probe is evenly spaced through the
/// disk's own taps rather than extra ones.
inline constexpr std::uint32_t kPcssProbeTaps = 4;

/// @brief How far from its own position a bilinear comparison reads, in texels,
/// as a bound: the four texel centres it blends all lie within one texel of it
/// along each axis.
inline constexpr float kCompareFootprintTexels = 1.f;

/// @brief The fraction of a unit-area tent of half-width @p halfWidth, centred
/// @p centre texels from the centre of texel 0, that falls on texel @p texel.
///
/// The exact integral over the texel, not a sample of the tent at its centre —
/// which is what makes the weights sum to one at every sub-texel position and
/// the edge move continuously as the lookup does.
[[nodiscard]] float TentTexelWeight(float centre, float halfWidth, std::int32_t texel);

/// @brief One bilinear tap along an axis: where it sits, in texels from the
/// centre of the texel the lookup falls in, and what it weighs.
struct TentAxisTap
{
    float position = 0.f;
    float weight = 0.f;
};

/// @brief The bilinear taps that reproduce a tent along one axis.
struct TentAxis
{
    std::array<TentAxisTap, kMaxTentAxisTaps> taps{};
    std::uint32_t count = 0;
};

/// @brief A tent of @p halfWidth texels centred @p fraction texels past the
/// centre of the lookup's own texel, as bilinear taps.
///
/// Adjacent texels are read in pairs: one bilinear fetch placed between their
/// centres at b / (a + b), weighted a + b, returns a c0 + b c1 for weights a and
/// b — so a 3-wide tent is 2 taps an axis and a 5-wide one 3, where the grid
/// they replace took 3 and 5.
/// The outermost tap sits at most half a texel short of the half-width, and its
/// blend reads half a texel past where it sits, so a tent reads no further from
/// its lookup than its half-width — which is what a tile's clamp inset needs.
/// @param fraction   In [0, 1).
[[nodiscard]] TentAxis TentAxisTaps(float fraction, float halfWidth);

/// @brief Whether a blocker @p gapDepth in front of the receiver, found
/// @p offsetUv from its lookup, shades it at all.
///
/// Only if its own penumbra reaches that far: an occluder whose shadow edge
/// falls short of the receiver is not occluding it. Counting it anyway is what
/// puts a tall caster's top into the estimate at its own base, and sizes the
/// widest kernel exactly where the shadow should be sharpest.
[[nodiscard]] bool PcssBlockerCounts(float penumbraUvPerDepth, float gapDepth, float offsetUv, float maxReachUv);

/// @brief How far the blocker search reaches from its lookup, in UV.
///
/// As far as the widest penumbra could — a blocker at the near plane — and never
/// less than the floor kernel reads: a search narrower than the kernel it sizes
/// finds nothing at the edge of a shadow the kernel would have softened.
[[nodiscard]] float PcssSearchRadiusUv(float penumbraUvPerDepth, float referenceDepth, float maxReachUv,
                                       float floorReachUv);

/// @brief How many taps the contact-hardening disk takes at penumbra radius
/// @p penumbraUv, or zero where the floor tent covers it.
///
/// Grows with the disk's area, so the taps per texel of penumbra hold roughly
/// still as it widens, up to kPcssMaxTaps. A multiple of kPcssProbeTaps.
[[nodiscard]] std::uint32_t PcssKernelTaps(float penumbraUv, float floorReachUv);

/// @brief The depth a receiver's plane can rise across one comparison's
/// footprint, for a plane of @p slope in depth per UV and a texel of
/// @p texelUv.
///
/// A tap tests the plane at its own position, but the hardware compares four
/// texels around it, and on a receiver the light grazes the plane climbs past
/// the bias between them — the floor then shadows itself in speckles.
/// Subtracting this covers every texel the comparison reads.
[[nodiscard]] float CompareFootprintDepth(float texelUv, const glm::vec2 &slope);

} // namespace Assisi::Render
