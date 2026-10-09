/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Geometry/AnimationBlend.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Geometry/Pose.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>

namespace Assisi::Geometry
{

namespace
{

/// A weight sum below this has no point to share it among: the parameter is
/// past every point at once, which only rounding produces.
constexpr float kNoWeight = 1e-6f;

/// How much point @p self counts for at @p parameter: 1 at the point, falling
/// along the line towards each other point to 0 there, the least over all of them.
float BandWeight(std::span<const glm::vec2> positions, std::size_t self, glm::vec2 parameter)
{
    const glm::vec2 point = positions[self];
    float weight = 1.f;
    for (std::size_t other = 0; other < positions.size(); ++other)
    {
        const glm::vec2 towards = positions[other] - point;
        const float spacing = glm::dot(towards, towards);
        if (other == self || spacing <= 0.f)
        {
            continue;
        }
        const float along = glm::dot(parameter - point, towards) / spacing;
        weight = std::min(weight, std::clamp(1.f - along, 0.f, 1.f));
    }
    return weight;
}

std::size_t NearestPoint(std::span<const glm::vec2> positions, glm::vec2 parameter)
{
    std::size_t nearest = 0;
    float nearestDistance = std::numeric_limits<float>::max();
    for (std::size_t point = 0; point < positions.size(); ++point)
    {
        const float distance = glm::distance(positions[point], parameter);
        if (distance < nearestDistance)
        {
            nearest = point;
            nearestDistance = distance;
        }
    }
    return nearest;
}

bool Weighted(const ClipBlend &blend, std::size_t source)
{
    return blend.Weights[source] >= kNegligibleWeight;
}

/// The one source with weight, if only one has any.
std::optional<std::size_t> SoleSource(const ClipBlend &blend)
{
    std::optional<std::size_t> sole;
    for (std::size_t source = 0; source < blend.Sources.size(); ++source)
    {
        if (!Weighted(blend, source))
        {
            continue;
        }
        if (sole.has_value())
        {
            return std::nullopt;
        }
        sole = source;
    }
    return sole;
}

float SourceTime(const ClipBlend &blend, std::size_t source)
{
    return blend.Phase * blend.Sources[source].Clip->Duration;
}

/// Marks in blend.Touched every joint a weighted source keys.
void MarkTouched(ClipBlend &blend, std::size_t jointCount)
{
    blend.Touched.assign(jointCount, 0);
    for (std::size_t source = 0; source < blend.Sources.size(); ++source)
    {
        if (!Weighted(blend, source))
        {
            continue;
        }
        for (const int32_t joint : blend.Sources[source].Binding.JointOfTrack)
        {
            if (joint != kNoJoint && static_cast<std::size_t>(joint) < jointCount)
            {
                blend.Touched[static_cast<std::size_t>(joint)] = 1;
            }
        }
    }
}

/// Adds @p share of @p local onto @p sum, its rotation turned onto the side of
/// the sphere the sum is on.
void Accumulate(const JointTransform &local, float share, JointTransform &sum)
{
    const glm::quat rotation = glm::dot(sum.Rotation, local.Rotation) < 0.f ? -local.Rotation : local.Rotation;
    sum.Rotation = sum.Rotation + rotation * share;
    sum.Translation += local.Translation * share;
    sum.Scale += local.Scale * share;
}

JointTransform Scaled(const JointTransform &local, float share)
{
    JointTransform scaled;
    scaled.Rotation = local.Rotation * share;
    scaled.Translation = local.Translation * share;
    scaled.Scale = local.Scale * share;
    return scaled;
}

float WeightedTotal(const ClipBlend &blend)
{
    float total = 0.f;
    for (std::size_t source = 0; source < blend.Sources.size(); ++source)
    {
        if (Weighted(blend, source))
        {
            total += blend.Weights[source];
        }
    }
    return total;
}

/// Writes @p share of the source sampled into blend.Scratch into the touched
/// joints of @p out: over them when @p first, onto them otherwise.
void AddScratch(const ClipBlend &blend, float share, bool first, std::span<JointTransform> out)
{
    for (std::size_t joint = 0; joint < out.size(); ++joint)
    {
        if (blend.Touched[joint] == 0)
        {
            continue;
        }
        if (first)
        {
            out[joint] = Scaled(blend.Scratch[joint], share);
        }
        else
        {
            Accumulate(blend.Scratch[joint], share, out[joint]);
        }
    }
}

} // namespace

void BlendWeights(std::span<const glm::vec2> positions, glm::vec2 parameter, std::span<float> weights)
{
    ASSISI_ASSERT(positions.size() == weights.size(), "a weight per point");
    float total = 0.f;
    for (std::size_t point = 0; point < positions.size(); ++point)
    {
        weights[point] = BandWeight(positions, point, parameter);
        total += weights[point];
    }
    if (total < kNoWeight)
    {
        std::ranges::fill(weights, 0.f);
        if (!weights.empty())
        {
            weights[NearestPoint(positions, parameter)] = 1.f;
        }
        return;
    }
    for (float &weight : weights)
    {
        weight /= total;
    }
}

void AdvancePhase(ClipBlend &blend, float dt, bool loop)
{
    ASSISI_ASSERT(blend.Weights.size() == blend.Sources.size(), "a weight per source");
    float length = 0.f;
    for (std::size_t source = 0; source < blend.Sources.size(); ++source)
    {
        length += blend.Weights[source] * blend.Sources[source].Clip->Duration;
    }
    if (length <= 0.f)
    {
        return;
    }
    blend.Phase = WrapClipTime(blend.Phase + dt / length, 1.f, loop);
}

void SampleBlend(ClipBlend &blend, const Skeleton &skeleton, std::span<JointTransform> out)
{
    ASSISI_ASSERT(blend.Weights.size() == blend.Sources.size(), "a weight per source");
    ASSISI_ASSERT(skeleton.RestLocal.size() == out.size(), "the pose is the skeleton's");

    // The common case, a clip on its own, writes only what it keys and costs
    // nothing over sampling the clip.
    if (const std::optional<std::size_t> sole = SoleSource(blend); sole.has_value())
    {
        const BlendSource &source = blend.Sources[*sole];
        SampleClip(*source.Clip, source.Binding, SourceTime(blend, *sole), out);
        return;
    }

    const float total = WeightedTotal(blend);
    if (total <= 0.f)
    {
        return;
    }
    MarkTouched(blend, out.size());
    blend.Scratch.resize(out.size());
    bool first = true;
    for (std::size_t source = 0; source < blend.Sources.size(); ++source)
    {
        if (!Weighted(blend, source))
        {
            continue;
        }
        std::ranges::copy(skeleton.RestLocal, blend.Scratch.begin());
        SampleClip(*blend.Sources[source].Clip, blend.Sources[source].Binding, SourceTime(blend, source),
                   blend.Scratch);
        AddScratch(blend, blend.Weights[source] / total, first, out);
        first = false;
    }
    for (std::size_t joint = 0; joint < out.size(); ++joint)
    {
        if (blend.Touched[joint] != 0)
        {
            out[joint].Rotation = glm::normalize(out[joint].Rotation);
        }
    }
}

void MixPoses(std::span<const JointTransform> from, std::span<const JointTransform> to, float weight,
              std::span<JointTransform> out)
{
    ASSISI_ASSERT(from.size() == out.size() && to.size() == out.size(), "three poses of one skeleton");
    for (std::size_t joint = 0; joint < out.size(); ++joint)
    {
        out[joint].Rotation = glm::normalize(glm::slerp(from[joint].Rotation, to[joint].Rotation, weight));
        out[joint].Translation = glm::mix(from[joint].Translation, to[joint].Translation, weight);
        out[joint].Scale = glm::mix(from[joint].Scale, to[joint].Scale, weight);
    }
}

float CrossFade::Weight() const
{
    if (Duration <= 0.f)
    {
        return 1.f;
    }
    return std::min(Elapsed / Duration, 1.f);
}

bool CrossFade::Done() const
{
    return Weight() >= 1.f;
}

void CrossFade::Advance(float dt)
{
    Elapsed += dt;
}

} // namespace Assisi::Geometry
