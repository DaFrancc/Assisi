/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ShadowFiltering.hpp>

#include <Assisi/Render/ShadowCascades.hpp>

#include <algorithm>
#include <cmath>

namespace Assisi::Render
{

namespace
{
/// The fraction of a unit-area tent of half-width @p halfWidth, centred on zero,
/// that lies below @p x.
float TentCdf(float x, float halfWidth)
{
    if (x <= -halfWidth)
    {
        return 0.f;
    }
    if (x >= halfWidth)
    {
        return 1.f;
    }
    const float twiceSquared = 2.f * halfWidth * halfWidth;
    if (x <= 0.f)
    {
        return (x + halfWidth) * (x + halfWidth) / twiceSquared;
    }
    return 1.f - (halfWidth - x) * (halfWidth - x) / twiceSquared;
}
} // namespace

float TentTexelWeight(float centre, float halfWidth, std::int32_t texel)
{
    // Texel k spans [k - 1/2, k + 1/2] around its own centre.
    const float low = static_cast<float>(texel) - 0.5f - centre;
    return TentCdf(low + 1.f, halfWidth) - TentCdf(low, halfWidth);
}

TentAxis TentAxisTaps(float fraction, float halfWidth)
{
    TentAxis axis;
    // The lowest texel the tent can touch, and from it 2h + 1 texels, read two at
    // a time. A pair the tent misses entirely still gets a tap, of weight zero,
    // so the count depends on the width alone and never on the position.
    const auto first = static_cast<std::int32_t>(std::floor(fraction - halfWidth + 0.5f));
    const auto pairs = std::min(static_cast<std::uint32_t>(halfWidth + 0.5f), kMaxTentAxisTaps);
    for (std::uint32_t pair = 0; pair < pairs; ++pair)
    {
        const std::int32_t texel = first + static_cast<std::int32_t>(2u * pair);
        const float a = TentTexelWeight(fraction, halfWidth, texel);
        const float b = TentTexelWeight(fraction, halfWidth, texel + 1);
        const float weight = a + b;
        axis.taps[axis.count++] =
            TentAxisTap{.position = static_cast<float>(texel) + (weight > 0.f ? b / weight : 0.f), .weight = weight};
    }
    return axis;
}

bool PcssBlockerCounts(float penumbraUvPerDepth, float gapDepth, float offsetUv, float maxReachUv)
{
    return gapDepth > 0.f && PcssPenumbraUv(penumbraUvPerDepth, gapDepth, maxReachUv) >= offsetUv;
}

float PcssSearchRadiusUv(float penumbraUvPerDepth, float referenceDepth, float maxReachUv, float floorReachUv)
{
    return std::max(PcssPenumbraUv(penumbraUvPerDepth, referenceDepth, maxReachUv), floorReachUv);
}

std::uint32_t PcssKernelTaps(float penumbraUv, float floorReachUv)
{
    if (!(penumbraUv > floorReachUv))
    {
        return 0u;
    }
    const float ratio = penumbraUv / floorReachUv;
    const float wanted = static_cast<float>(kPcssMinTaps) * ratio * ratio;
    const float probes = static_cast<float>(kPcssProbeTaps);
    // Floor of x + 1/2 rather than std::round: GLSL leaves which way round()
    // breaks a tie to the implementation, and the shader must agree with this.
    const float rounded = std::floor(std::min(wanted, static_cast<float>(kPcssMaxTaps)) / probes + 0.5f) * probes;
    return std::clamp(static_cast<std::uint32_t>(rounded), kPcssMinTaps, kPcssMaxTaps);
}

float CompareFootprintDepth(float texelUv, const glm::vec2 &slope)
{
    return kCompareFootprintTexels * texelUv * (std::abs(slope.x) + std::abs(slope.y));
}

} // namespace Assisi::Render
