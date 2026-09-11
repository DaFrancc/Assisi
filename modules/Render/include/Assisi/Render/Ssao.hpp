/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Ssao.hpp
/// @brief The math of screen-space ambient occlusion: the kernel, the per-pixel
/// rotation, and the depth arithmetic the passes share.
///
/// Every function here has a transcription in ssao_depth.frag, ssao.frag,
/// ssao_blur.frag or mesh.frag, and the two must agree — this side exists so the
/// claims the shaders rest on can be checked without a GPU.
///
/// The whole technique is spatial. Nothing reads a previous frame, so a pixel's
/// answer is a pure function of this frame's depth: nothing can flicker that the
/// depth itself does not.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/IndirectLighting.hpp>
#include <Assisi/Render/SsaoSettings.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Assisi::Render
{

/// @brief The side of the square the per-pixel rotation repeats over.
///
/// Sixteen rotations of one kernel are sixteen times its samples once the blur
/// has averaged them, which is what lets a dozen samples a pixel read as smooth.
inline constexpr uint32_t kSsaoTileSize = 4;
inline constexpr uint32_t kSsaoTileRotationCount = kSsaoTileSize * kSsaoTileSize;

/// @brief Which of the sixteen rotations each pixel of the tile takes, row by
/// row. Ordered-dither order, so neighbours inside the tile are far apart in
/// angle and a crease sampled by one pixel is searched from elsewhere by the
/// next. ssao.frag carries the same table.
inline constexpr std::array<uint32_t, kSsaoTileRotationCount> kSsaoTileRotation = {
    0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

/// @brief Where each blur tap sits relative to the pixel, along the pass's axis.
///
/// One full period of the tile, so the horizontal and vertical passes between
/// them average all sixteen rotations exactly once and the pattern cancels
/// rather than softening. The half-pixel lean toward the negative side is the
/// price of an even period.
inline constexpr std::array<int32_t, kSsaoTileSize> kSsaoBlurTapOffsets = {-2, -1, 0, 1};

/// @brief The shortest sample, as a fraction of the radius.
///
/// Samples crowd toward the point, where occlusion is decided, but not onto it:
/// a sample at the surface itself tests the depth it was reconstructed from.
inline constexpr float kSsaoKernelMinScale = 0.1f;

/// @brief The largest squared sine a sample may make with the surface normal.
///
/// Keeps every sample about eighteen degrees clear of the tangent plane. A normal
/// rebuilt from depth is off by a little everywhere, and a sample lying in the
/// plane it tilts through would find a flat floor occluding itself.
inline constexpr float kSsaoKernelMaxSinSquared = 0.9f;

/// @brief The widest the radius may project, in pixels.
///
/// A metre-wide radius on a wall a hand's breadth away reaches across the whole
/// screen, and every sample a pixel takes then misses the texture cache. Past
/// this the radius shrinks with distance instead, so the cost of a close wall is
/// the cost of any other wall.
inline constexpr float kSsaoMaxRadiusPixels = 128.0f;

/// @brief How far in front of a sample the depth buffer must be before it
/// counts as an occluder, as a fraction of the radius.
///
/// What stops a flat surface from shadowing itself through the quantisation of
/// its own depth and the error in the normal rebuilt from it.
inline constexpr float kSsaoBiasFraction = 0.05f;

/// @brief The largest relative difference in distance at which a blur tap still
/// counts as the same surface as the pixel it blurs into.
///
/// A step in depth larger than this is a silhouette, and occlusion carried across
/// one is the halo round every object's outline.
inline constexpr float kSsaoBlurDepthTolerance = 0.05f;

/// @brief The four projection terms the passes reconstruct view space from.
///
/// For a perspective projection with depth in [0, 1] and the camera looking down
/// -Z: the two lens scales, and the pair that maps a stored depth back to a
/// distance. No reversed Z; a depth of one is the far plane.
struct SsaoProjection
{
    float xScale = 1.0f;     ///< projection[0][0] — one over (aspect x tan(fov/2))
    float yScale = 1.0f;     ///< projection[1][1] — one over tan(fov/2)
    float depthScale = 0.0f; ///< projection[3][2]
    float depthBias = 0.0f;  ///< projection[2][2]
};

[[nodiscard]] inline SsaoProjection MakeSsaoProjection(const glm::mat4 &projection)
{
    return SsaoProjection{.xScale = projection[0][0],
                          .yScale = projection[1][1],
                          .depthScale = projection[3][2],
                          .depthBias = projection[2][2]};
}

/// @brief The distance in front of the camera a stored depth came from.
///
/// Inverts clip z / clip w = (depthBias * z + depthScale) / -z for z, which is
/// the whole of ssao_depth.frag.
[[nodiscard]] inline float SsaoLinearDepth(float depth, const SsaoProjection &projection)
{
    return projection.depthScale / (depth + projection.depthBias);
}

/// @brief The view-space point at @p distance in front of the camera through the
/// centre of @p pixel, counting rows from the top of the target.
///
/// The top row is NDC +1 because the viewport the scene draws through is
/// flipped; see fullscreen.vert for the same convention.
[[nodiscard]] inline glm::vec3 SsaoViewPosition(const glm::vec2 &pixel, float distance, const glm::vec2 &size,
                                                const SsaoProjection &projection)
{
    const glm::vec2 ndc((pixel.x + 0.5f) / size.x * 2.0f - 1.0f, 1.0f - (pixel.y + 0.5f) / size.y * 2.0f);
    return glm::vec3(ndc.x / projection.xScale * distance, ndc.y / projection.yScale * distance, -distance);
}

/// @brief Where on the target the view-space point @p position lands, in pixels
/// from the top-left corner. SsaoViewPosition's inverse.
[[nodiscard]] inline glm::vec2 SsaoPixelOf(const glm::vec3 &position, const glm::vec2 &size,
                                           const SsaoProjection &projection)
{
    const float distance = -position.z;
    const glm::vec2 ndc(position.x * projection.xScale / distance, position.y * projection.yScale / distance);
    return glm::vec2((ndc.x + 1.0f) * 0.5f * size.x, (1.0f - ndc.y) * 0.5f * size.y) - glm::vec2(0.5f);
}

/// @brief The radius a pixel at @p distance searches, in metres: @p radius, or
/// less where that would project wider than kSsaoMaxRadiusPixels.
[[nodiscard]] inline float SsaoEffectiveRadius(float radius, float distance, float height,
                                               const SsaoProjection &projection)
{
    const float pixelsPerMetre = projection.yScale * 0.5f * height / distance;
    return std::min(radius, kSsaoMaxRadiusPixels / pixelsPerMetre);
}

/// @brief The rotation about the normal that pixel (@p x, @p y) turns the
/// kernel through, in radians.
///
/// A function of the pixel's place on the screen and nothing else, so it is the
/// same every frame; the blur is what removes it.
[[nodiscard]] inline float SsaoTileAngle(uint32_t x, uint32_t y)
{
    const uint32_t index = kSsaoTileRotation[(y % kSsaoTileSize) * kSsaoTileSize + (x % kSsaoTileSize)];
    return static_cast<float>(index) * glm::two_pi<float>() / static_cast<float>(kSsaoTileRotationCount);
}

/// @brief The van der Corput sequence in base two: the bits of @p index mirrored
/// about the binary point.
///
/// Each prefix of it is spread over [0, 1) about as evenly as that many points
/// can be, which is what keeps a kernel of any length from leaving a band of
/// elevations empty.
[[nodiscard]] inline float RadicalInverseBase2(uint32_t index)
{
    // One over 2^32, which is where the reversed bits are read from.
    constexpr float kTwoToMinus32 = 2.3283064365386963e-10f;
    uint32_t bits = index;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * kTwoToMinus32;
}

/// @brief @p count sample offsets in the hemisphere about +Z, as fractions of
/// the radius.
///
/// Deterministic — no random source anywhere — so the same settings give the same
/// kernel and nothing changes between frames. Elevations are cosine-distributed,
/// so a plain count of occluded samples is already the cosine-weighted fraction a
/// Lambertian surface would see blocked. Azimuths step by the golden angle and
/// elevations by the van der Corput sequence, two orders that do not correlate,
/// so the short samples are not all the steep ones. Lengths grow with the index,
/// squared, which crowds samples toward the point.
[[nodiscard]] inline std::vector<glm::vec3> BuildSsaoKernel(uint32_t count)
{
    std::vector<glm::vec3> kernel;
    kernel.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const float sinSquared = RadicalInverseBase2(i) * kSsaoKernelMaxSinSquared;
        const float sinZenith = std::sqrt(sinSquared);
        const float cosZenith = std::sqrt(1.0f - sinSquared);
        const float azimuth = static_cast<float>(i) * kGoldenAngle;
        const glm::vec3 direction(std::cos(azimuth) * sinZenith, std::sin(azimuth) * sinZenith, cosZenith);

        const float t = static_cast<float>(i + 1u) / static_cast<float>(count);
        kernel.push_back(direction * glm::mix(kSsaoKernelMinScale, 1.0f, t * t));
    }
    return kernel;
}

/// @brief How much an occluder found @p depthDelta metres from the point counts,
/// for a search of @p radius metres.
///
/// Full weight within the radius and falling away past it. Without it, a pixel
/// on the ground behind a pillar finds the pillar metres in front of it and draws
/// a dark outline on the ground around the pillar's silhouette.
[[nodiscard]] inline float SsaoRangeWeight(float radius, float depthDelta)
{
    const float ratio = radius / std::max(std::abs(depthDelta), radius * kSsaoBiasFraction);
    const float t = std::clamp(ratio, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// @brief How much a blur tap at @p tapDistance contributes to a pixel at
/// @p centerDistance: one on the same surface, falling to zero at a step of
/// kSsaoBlurDepthTolerance of the distance and beyond.
[[nodiscard]] inline float SsaoBlurWeight(float centerDistance, float tapDistance)
{
    const float step = std::abs(tapDistance - centerDistance) / (kSsaoBlurDepthTolerance * centerDistance);
    return std::clamp(1.0f - step, 0.0f, 1.0f);
}

/// @brief The share of a reflection that survives an ambient occlusion of
/// @p ao, for a lobe of GGX alpha @p alpha seen at @p nDotV.
///
/// Lagarde and de Rousiers' fit. A rough lobe spans most of the hemisphere and
/// takes nearly the diffuse answer; a sharp one looking straight out of a crease
/// sees open sky and keeps nearly all of it. Exactly one at full visibility and
/// zero at none, whatever the lobe.
[[nodiscard]] inline float SpecularOcclusion(float nDotV, float ao, float alpha)
{
    // The fit's own constants: how quickly the exponent falls with roughness,
    // and where it starts for a mirror.
    constexpr float kExponentPerAlpha = -16.0f;
    constexpr float kMirrorExponent = -1.0f;
    const float exponent = std::exp2(kExponentPerAlpha * alpha + kMirrorExponent);
    return std::clamp(std::pow(nDotV + ao, exponent) - 1.0f + ao, 0.0f, 1.0f);
}

} // namespace Assisi::Render
