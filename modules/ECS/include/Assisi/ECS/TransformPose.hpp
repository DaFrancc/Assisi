/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TransformPose.hpp
/// @brief Resolving a local Transform against a parent's world matrix — the
/// rigid half of it, position and rotation, with scale left out.
///
/// This is how a physics body is placed: Jolt works in world space, a parented
/// Transform is an offset from its parent, and a collider's dimensions are
/// absolute world units that no scale may stretch. Anything drawing where a body
/// actually is (the editor's collider overlays) has to compose it the same way,
/// and a second copy of the rule is a second thing to keep in step.
///
/// It lives in ECS because the two users cannot reach each other: Physics does
/// not link Runtime, and the Editor does not see inside PhysicsWorld.cpp.

#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::ECS
{

/// @brief @p world's rotation alone, with scale divided out of each basis vector.
///
/// Exact for uniform scale, which is what a blueprint instance root is
/// constrained to — and a non-uniformly
/// scaled matrix has no exact rotation to extract in the first place, because the
/// composition is a shear rather than a TRS.
[[nodiscard]] inline glm::quat WorldRotationOf(const glm::mat4 &world)
{
    const glm::mat3 basis(world);
    return glm::quat_cast(
        glm::mat3(glm::normalize(basis[0]), glm::normalize(basis[1]), glm::normalize(basis[2])));
}

/// @brief @p local placed into world space under @p parentWorld.
///
/// Position and rotation only: @p local's scale is carried through untouched
/// rather than composed, because the callers place rigid bodies, whose collider
/// dimensions do not scale with the entity. The returned Transform's worldMatrix
/// is not filled in — this answers a question, it does not stand in for
/// propagation.
[[nodiscard]] inline Transform PoseUnderParent(const Transform &local, const glm::mat4 &parentWorld)
{
    Transform out;
    out.position = glm::vec3(parentWorld * glm::vec4(local.position, 1.f));
    out.rotation = glm::normalize(WorldRotationOf(parentWorld) * local.rotation);
    out.scale    = local.scale;
    return out;
}

} // namespace Assisi::ECS
