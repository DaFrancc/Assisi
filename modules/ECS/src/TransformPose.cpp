/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/ECS/TransformPose.hpp>

#include <cmath>

namespace Assisi::ECS
{

bool HasUniformScale(const Transform &transform)
{
    // Relative, so a metre-scale and a kilometre-scale instance are held to the
    // same standard. The tolerance exists for a hand-typed 1.0000001, not to let a
    // genuinely non-uniform scale through: the smallest visible non-uniformity is
    // orders of magnitude above this.
    constexpr float kTolerance = 1e-5f;

    const glm::vec3 &scale = transform.scale;
    const float mean = (std::abs(scale.x) + std::abs(scale.y) + std::abs(scale.z)) / 3.f;
    if (mean <= 0.f)
    {
        return scale.x == scale.y && scale.y == scale.z;
    }

    return std::abs(scale.x - scale.y) / mean < kTolerance && std::abs(scale.y - scale.z) / mean < kTolerance;
}

Transform ComposeTransform(const Transform &placement, const Transform &local)
{
    Transform out;
    out.position = placement.position + (placement.rotation * (placement.scale * local.position));
    out.rotation = glm::normalize(placement.rotation * local.rotation);
    out.scale = placement.scale * local.scale;
    return out;
}

Transform InverseComposeTransform(const Transform &placement, const Transform &world)
{
    // Exact only under the uniform-scale rule, same as the forward form: with one
    // scale factor the division below is a scalar and the rotation is unaffected
    // by it.
    const float scale = placement.scale.x != 0.f ? placement.scale.x : 1.f;

    Transform out;
    out.rotation = glm::normalize(glm::inverse(placement.rotation) * world.rotation);
    out.position = (glm::inverse(placement.rotation) * (world.position - placement.position)) / scale;
    out.scale = world.scale / scale;
    return out;
}

} // namespace Assisi::ECS
