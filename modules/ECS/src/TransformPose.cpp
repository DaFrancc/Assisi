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

namespace
{

/// @p placement's position plus @p offset turned and scaled by it, in doubles so
/// a placement far from the world's origin loses nothing.
glm::dvec3 PlacedPosition(const Transform &placement, const glm::dvec3 &offset)
{
    return placement.position + glm::dquat(placement.rotation) * (glm::dvec3(placement.scale) * offset);
}

/// The offset PlacedPosition would have turned into @p position.
glm::dvec3 OffsetUnder(const Transform &placement, const glm::dvec3 &position)
{
    return (glm::inverse(glm::dquat(placement.rotation)) * (position - placement.position)) /
           glm::dvec3(placement.scale);
}

} // namespace

Transform ComposeTransform(const Transform &placement, const Transform &local)
{
    Transform out;
    out.position = PlacedPosition(placement, local.position);
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
    out.position = (glm::inverse(glm::dquat(placement.rotation)) * (world.position - placement.position)) /
                   static_cast<double>(scale);
    out.scale = world.scale / scale;
    return out;
}

Transform PoseUnderParent(const Transform &local, const Transform &parentWorld)
{
    Transform out;
    out.position = PlacedPosition(parentWorld, local.position);
    out.rotation = glm::normalize(parentWorld.rotation * local.rotation);
    out.scale = local.scale;
    return out;
}

Transform PoseInParent(const Transform &world, const Transform &parentWorld)
{
    Transform out;
    out.position = OffsetUnder(parentWorld, world.position);
    out.rotation = glm::normalize(glm::inverse(parentWorld.rotation) * world.rotation);
    out.scale = world.scale;
    return out;
}

} // namespace Assisi::ECS
