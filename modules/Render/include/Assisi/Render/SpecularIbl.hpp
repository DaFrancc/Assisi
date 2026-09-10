/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SpecularIbl.hpp
/// @brief The GGX lobe's integrals: the split-sum approximation's two halves,
/// the per-light lobe's directional albedo, and the cube map layout the
/// environment half is stored in.
///
/// A glossy surface reflects the environment through its GGX lobe. The split
/// sum factors that integral into what the environment sends along the lobe
/// (@ref PrefilterGgx, baked per roughness into a cube map's mips) and how much
/// of it the lobe reflects (@ref IntegrateEnvBrdf). Both are pure functions
/// here, and the GPU halves — sky_prefilter.comp and the BRDF table MeshPass
/// uploads — transcribe them.
///
/// **Two Smith terms, one per lobe.** mesh.frag's CookTorrance takes
/// k = (roughness + 1)^2 / 8, a fit tuned for punctual lights; integrating over
/// an environment wants k = alpha / 2. Each lobe's multi-scatter compensation
/// has to come from its own directional albedo, which is why the table carries
/// the per-light lobe's (@ref IntegrateDirectAlbedo) beside the split sum's.

#include <Assisi/Math/GLM.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Assisi::Render
{

/// @brief Side of the BRDF table. The table is smooth in both axes and read
/// with bilinear filtering, so a small one loses nothing visible.
inline constexpr uint32_t kBrdfLutSize = 32;

/// @brief Samples per texel of the BRDF table. It is built once, on the CPU,
/// so this is startup time rather than frame time.
inline constexpr uint32_t kBrdfLutSampleCount = 512;

/// @brief The most samples any one prefilter texel takes.
inline constexpr uint32_t kMaxPrefilterSampleCount = 1024;

inline constexpr uint32_t kCubeFaceCount = 6;

/// @brief Each blurred mip takes this many times the samples of the one above
/// it. A mip has a quarter of the texels of its parent, so this holds the work
/// per mip level while the lobe widens.
inline constexpr uint32_t kPrefilterSampleGrowth = 4;

/// @brief The smallest face a probe keeps a mip for. Below it a face holds too
/// few texels to say which way the lobe points.
inline constexpr uint32_t kMinProbeMipSize = 8;

/// @brief Bits of a float's significand, counting the implicit one: the most
/// an integer can carry into a float and come out exact.
inline constexpr uint32_t kFloatSignificandBits = 24;

/// @brief How close to the normal the ImportanceSampleGgx helper axis may lie
/// before the other one is taken, as a cosine. Past it their cross product is
/// too short to normalise accurately.
inline constexpr float kMaxHelperAxisCosine = 0.999f;

/// @brief Smallest n.v the split-sum integral is evaluated at. At zero the view
/// lies in the surface, and the estimator divides by n.v.
inline constexpr float kMinEnvBrdfNdotV = 1e-4f;

/// @brief Schlick's Fresnel exponent.
inline constexpr float kSchlickExponent = 5.0f;

/// @brief Near and far planes of a cube face's capture projection. Only the
/// directions the projection produces are used, so any positive pair will do.
inline constexpr float kCubeCaptureNear = 0.1f;
inline constexpr float kCubeCaptureFar = 10.0f;

/// @brief @p bits mirrored about the binary point: the van der Corput sequence,
/// in [0, 1). sky_prefilter.comp's bitfieldReverse is the same operation.
///
/// Only the top kFloatSignificandBits mirrored bits are kept, because that is
/// what a float holds exactly: converting all 32 rounds the largest values up
/// to one, outside the range the GGX inversion is defined on.
[[nodiscard]] inline float RadicalInverse(uint32_t bits)
{
    // Swap halves, then bytes' nibbles, pairs and bits: a 32-bit reversal in
    // five steps, each exchanging neighbouring groups of the width it shifts.
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);

    constexpr uint32_t kDroppedBits = 32u - kFloatSignificandBits;
    constexpr float kUnitScale = 1.0f / static_cast<float>(1u << kFloatSignificandBits);
    return static_cast<float>(bits >> kDroppedBits) * kUnitScale;
}

/// @brief Point @p index of a @p count-point Hammersley set in the unit square.
///
/// Low-discrepancy rather than random, so the same count always gives the same
/// answer: a bake that is repeated for an unchanged sky produces the same image,
/// and a test can hold the result to a tolerance rather than to a distribution.
[[nodiscard]] inline glm::vec2 Hammersley(uint32_t index, uint32_t count)
{
    return glm::vec2(static_cast<float>(index) / static_cast<float>(count), RadicalInverse(index));
}

/// @brief A half vector drawn from the GGX distribution about @p normal, with
/// density D(h) * (n . h).
///
/// @param alpha  GGX alpha, the square of perceptual roughness. Zero returns
///               the normal itself.
[[nodiscard]] inline glm::vec3 ImportanceSampleGgx(const glm::vec2 &xi, float alpha, const glm::vec3 &normal)
{
    const float a2 = alpha * alpha;
    const float phi = glm::two_pi<float>() * xi.x;
    const float cosTheta = std::sqrt((1.0f - xi.y) / (1.0f + (a2 - 1.0f) * xi.y));
    const float sinTheta = std::sqrt(std::max(1.0f - cosTheta * cosTheta, 0.0f));

    // Any frame about the normal will do, since the distribution is isotropic;
    // the only requirement is that the helper axis is not parallel to it.
    const glm::vec3 helper =
        std::abs(normal.z) < kMaxHelperAxisCosine ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 tangent = glm::normalize(glm::cross(helper, normal));
    const glm::vec3 bitangent = glm::cross(normal, tangent);

    return glm::normalize(tangent * (sinTheta * std::cos(phi)) + bitangent * (sinTheta * std::sin(phi)) +
                          normal * cosTheta);
}

/// @brief One direction's Smith-Schlick masking for an environment, k = alpha / 2.
[[nodiscard]] inline float IblSmithG1(float nDotX, float alpha)
{
    const float k = alpha * 0.5f;
    return nDotX / (nDotX * (1.0f - k) + k);
}

/// @brief One direction's Smith-Schlick masking as mesh.frag's per-light lobe
/// computes it, k = (roughness + 1)^2 / 8.
///
/// @param roughness  Perceptual roughness, not alpha: the per-light fit is
///                   written in it.
[[nodiscard]] inline float DirectSmithG1(float nDotX, float roughness)
{
    const float r = roughness + 1.0f;
    constexpr float kDirectSmithDivisor = 8.0f;
    const float k = r * r / kDirectSmithDivisor;
    return nDotX / (nDotX * (1.0f - k) + k);
}

/// @brief One sample of a GGX lobe seen from @p nDotV, drawn from the
/// importance-sampled half vector @p half: n.l, n.h and v.h, or n.l of zero
/// for a light below the surface.
struct GgxSample
{
    float nDotL = 0.0f;
    float nDotH = 0.0f;
    float vDotH = 0.0f;
};

[[nodiscard]] inline GgxSample SampleGgxLobe(float nDotV, float alpha, const glm::vec2 &xi)
{
    const glm::vec3 normal(0.0f, 0.0f, 1.0f);
    const glm::vec3 view(std::sqrt(1.0f - nDotV * nDotV), 0.0f, nDotV);
    const glm::vec3 half = ImportanceSampleGgx(xi, alpha, normal);
    const float vDotH = glm::dot(view, half);
    const glm::vec3 light = 2.0f * vDotH * half - view;
    return GgxSample{.nDotL = std::clamp(light.z, 0.0f, 1.0f),
                     .nDotH = std::clamp(half.z, 0.0f, 1.0f),
                     .vDotH = std::clamp(vDotH, 0.0f, 1.0f)};
}

/// @brief The split sum's second half: the scale and bias (A, B) such that a
/// GGX lobe with Schlick Fresnel reflects F0 * A + B of a uniform environment.
///
/// A + B is the lobe's directional albedo at F0 = 1, which is never above one;
/// what it falls short by is the energy a single scattering loses, and the
/// multi-scatter compensation in mesh.frag puts that back.
///
/// @param nDotV      Cosine between the view and the normal; held off zero.
/// @param roughness  Perceptual roughness.
[[nodiscard]] inline glm::vec2 IntegrateEnvBrdf(float nDotV, float roughness, uint32_t sampleCount)
{
    const float nv = std::clamp(nDotV, kMinEnvBrdfNdotV, 1.0f);
    const float alpha = roughness * roughness;

    glm::vec2 sum(0.0f);
    for (uint32_t i = 0; i < sampleCount; ++i)
    {
        const GgxSample s = SampleGgxLobe(nv, alpha, Hammersley(i, sampleCount));
        if (s.nDotL <= 0.0f)
        {
            continue;
        }
        // The estimator divides the BRDF by the pdf the half vector was drawn
        // with, D (n.h) / (4 v.h); D cancels, which is why no D appears.
        const float g = IblSmithG1(nv, alpha) * IblSmithG1(s.nDotL, alpha);
        const float gVis = g * s.vDotH / (s.nDotH * nv);
        const float fc = std::pow(1.0f - s.vDotH, kSchlickExponent);
        sum += glm::vec2((1.0f - fc) * gVis, fc * gVis);
    }
    return sum / static_cast<float>(sampleCount);
}

/// @brief The directional albedo of mesh.frag's per-light lobe at F0 = 1: the
/// fraction of light arriving from every direction that the single-scattering
/// lobe reflects toward @p nDotV.
///
/// What it falls short of one by is the energy lost to light that would have
/// bounced between microfacets again; CookTorrance scales its lobe by
/// 1 + F0 (1 / this - 1) to put that back. The same integral as the split
/// sum's A + B, with the per-light Smith term in place of the environment's.
[[nodiscard]] inline float IntegrateDirectAlbedo(float nDotV, float roughness, uint32_t sampleCount)
{
    const float nv = std::clamp(nDotV, kMinEnvBrdfNdotV, 1.0f);
    const float alpha = roughness * roughness;

    float sum = 0.0f;
    for (uint32_t i = 0; i < sampleCount; ++i)
    {
        const GgxSample s = SampleGgxLobe(nv, alpha, Hammersley(i, sampleCount));
        if (s.nDotL <= 0.0f)
        {
            continue;
        }
        const float g = DirectSmithG1(nv, roughness) * DirectSmithG1(s.nDotL, roughness);
        sum += g * s.vDotH / (s.nDotH * nv);
    }
    return sum / static_cast<float>(sampleCount);
}

/// @brief One texel of the BRDF table.
struct BrdfTableTexel
{
    /// The split sum's scale and bias: F0 * A + B of an environment is what
    /// the lobe reflects.
    glm::vec2 environment{0.0f};
    /// @ref IntegrateDirectAlbedo, for the per-light lobe's compensation.
    float directAlbedo = 0.0f;
};

/// @brief The BRDF table, row-major: column is n.v and row is roughness, each
/// sampled at its texel's centre so bilinear filtering reads it back exactly
/// where it was taken.
[[nodiscard]] inline std::vector<BrdfTableTexel> BuildBrdfLut(uint32_t size, uint32_t sampleCount)
{
    std::vector<BrdfTableTexel> table(static_cast<size_t>(size) * size);
    for (uint32_t row = 0; row < size; ++row)
    {
        const float roughness = (static_cast<float>(row) + 0.5f) / static_cast<float>(size);
        for (uint32_t column = 0; column < size; ++column)
        {
            const float nDotV = (static_cast<float>(column) + 0.5f) / static_cast<float>(size);
            table[static_cast<size_t>(row) * size + column] =
                BrdfTableTexel{.environment = IntegrateEnvBrdf(nDotV, roughness, sampleCount),
                               .directAlbedo = IntegrateDirectAlbedo(nDotV, roughness, sampleCount)};
        }
    }
    return table;
}

/// @brief The split sum's first half: the mean radiance @p environment sends
/// along a GGX lobe about @p reflection, weighted by n . l.
///
/// The lobe is taken with the normal and the view both equal to the reflection,
/// which is what lets one value per direction serve every view; the price is a
/// lobe that does not stretch at grazing angles, and the split sum accepts it.
///
/// @param environment  Any callable from a unit direction to a radiance.
/// @param roughness    Perceptual roughness. Zero is the environment at
///                     @p reflection, unblurred.
template <typename Environment>
[[nodiscard]] glm::vec3 PrefilterGgx(const Environment &environment, const glm::vec3 &reflection, float roughness,
                                     uint32_t sampleCount)
{
    const glm::vec3 axis = glm::normalize(reflection);
    const float alpha = roughness * roughness;

    glm::vec3 sum(0.0f);
    float weight = 0.0f;
    for (uint32_t i = 0; i < sampleCount; ++i)
    {
        const glm::vec3 half = ImportanceSampleGgx(Hammersley(i, sampleCount), alpha, axis);
        const glm::vec3 light = 2.0f * glm::dot(axis, half) * half - axis;
        const float nDotL = glm::dot(axis, light);
        if (nDotL > 0.0f)
        {
            sum += glm::vec3(environment(light)) * nDotL;
            weight += nDotL;
        }
    }
    return weight > 0.0f ? sum / weight : glm::vec3(environment(axis));
}

/// @brief How many mips a probe of @p resolution keeps: down to
/// kMinProbeMipSize on a side.
[[nodiscard]] inline uint32_t SkyProbeMipCount(uint32_t resolution)
{
    uint32_t mips = 0;
    for (uint32_t size = resolution; size >= kMinProbeMipSize; size >>= 1u)
    {
        ++mips;
    }
    return std::max(mips, 1u);
}

/// @brief The perceptual roughness @p mip is filtered for: zero at the top, one
/// at the last, linear between. mesh.frag reads the level back as roughness
/// times the last mip's index, so the two must stay each other's inverse.
[[nodiscard]] inline float PrefilterRoughness(uint32_t mip, uint32_t mipCount)
{
    return mipCount > 1u ? static_cast<float>(mip) / static_cast<float>(mipCount - 1u) : 0.0f;
}

/// @brief Samples per texel for @p mip: @p baseSampleCount at the first blurred
/// mip, kPrefilterSampleGrowth times more at each after it, up to
/// kMaxPrefilterSampleCount. Mip 0 is roughness zero, whose lobe is a single
/// direction.
[[nodiscard]] inline uint32_t PrefilterSampleCount(uint32_t mip, uint32_t baseSampleCount)
{
    if (mip == 0u)
    {
        return 1u;
    }
    uint64_t count = std::max(baseSampleCount, 1u);
    for (uint32_t i = 1u; i < mip && count < kMaxPrefilterSampleCount; ++i)
    {
        count *= kPrefilterSampleGrowth;
    }
    return static_cast<uint32_t>(std::min<uint64_t>(count, kMaxPrefilterSampleCount));
}

/// @brief One face of a cube map: the axis through its centre, and the
/// directions its two texture coordinates run along.
struct CubeFaceBasis
{
    glm::vec3 axis;
    glm::vec3 s; ///< Where increasing u points.
    glm::vec3 t; ///< Where increasing v points; v = 0 is the face's first row.
};

/// @brief The six faces in layer order +X, -X, +Y, -Y, +Z, -Z, as Vulkan's
/// cube sampling selects them. Every cube this engine writes — by rasterizing
/// or by a compute store — has to land its texels where this says, or the
/// sampler reads a face mirrored or turned.
[[nodiscard]] inline CubeFaceBasis CubeFace(uint32_t face)
{
    static constexpr std::array<CubeFaceBasis, kCubeFaceCount> kFaces{{
        {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, -1.0f, 0.0f}},
        {{-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, -1.0f, 0.0f}},
        {{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
        {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}},
        {{0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}},
    }};
    return kFaces[std::min(face, kCubeFaceCount - 1u)];
}

/// @brief The unit direction the texel at @p uv of @p face is sampled from.
/// sky_prefilter.comp's FaceDirection transcribes this.
[[nodiscard]] inline glm::vec3 CubeFaceDirection(uint32_t face, const glm::vec2 &uv)
{
    const CubeFaceBasis basis = CubeFace(face);
    return glm::normalize(basis.axis + basis.s * (2.0f * uv.x - 1.0f) + basis.t * (2.0f * uv.y - 1.0f));
}

/// @brief The view-projection that rasterizes @p face of a cube from its centre.
///
/// Built from the face's own basis rather than from lookAt, because a cube face
/// is mirrored against a right-handed camera and no choice of up vector fixes
/// that. The backend also draws with a flipped viewport, so a framebuffer's
/// first row is NDC y = +1 — which makes screen-up the face's -t. The field of
/// view is a right angle because that is what one face of a cube subtends.
[[nodiscard]] inline glm::mat4 CubeFaceViewProjection(uint32_t face)
{
    const CubeFaceBasis basis = CubeFace(face);
    const glm::vec3 right = basis.s;
    const glm::vec3 up = -basis.t;
    const glm::vec3 back = -basis.axis;

    // Rows of the view rotation; glm indexes column first.
    glm::mat4 view(1.0f);
    for (int32_t i = 0; i < 3; ++i)
    {
        view[i][0] = right[i];
        view[i][1] = up[i];
        view[i][2] = back[i];
    }
    constexpr float kSquareAspect = 1.0f;
    return glm::perspective(glm::half_pi<float>(), kSquareAspect, kCubeCaptureNear, kCubeCaptureFar) * view;
}

} // namespace Assisi::Render
