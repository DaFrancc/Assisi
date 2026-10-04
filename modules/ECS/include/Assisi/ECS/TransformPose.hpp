/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TransformPose.hpp
/// @brief Composing Transforms: one TRS placed under another, and a local
/// Transform resolved against a parent's world matrix.
///
/// The TRS composition is how a blueprint instance reaches its members and how
/// WorldTransformOf walks a parent chain; host and client both call it, so it is
/// defined once. PoseUnderParent is the rigid half — position and rotation, scale
/// left out — which is how a physics body is placed: Jolt works in world space and
/// a collider's dimensions are absolute world units that no scale may stretch.
/// Anything drawing where a body actually is (the editor's collider overlays) has
/// to compose it the same way.

#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::ECS
{

/// @brief Composes @p placement onto @p local, the way an instance's root reaches
/// a member and a parent reaches its child.
///
/// Exact only when @p placement's scale is uniform: uniform scale commutes with
/// rotation, so the product decomposes cleanly back to TRS. A non-uniform scale
/// introduces shear, and the result cannot be represented as a Transform at all —
/// which is why a blueprint file carrying one fails the load rather than being
/// clamped.
///
/// **One function, called by both sides.** Client blueprint expansion has to agree
/// with host expansion field for field, because the first snapshot is a delta
/// against the blueprint; two spellings of this that differ in the low bits are a
/// silent cross-build desync.
[[nodiscard]] Transform ComposeTransform(const Transform &placement, const Transform &local);

/// @brief The exact inverse: what @p local would have to be for
/// `ComposeTransform(placement, local)` to equal @p world.
///
/// This is how an override is recorded relative to its instance, and how "create
/// a blueprint from this selection" writes members around the new file's own
/// origin rather than around wherever they happened to be standing. Kept beside
/// its forward form on purpose — the two must agree to the bit, and they are the
/// pair a cross-build desync would come from.
[[nodiscard]] Transform InverseComposeTransform(const Transform &placement, const Transform &world);

/// @brief Whether @p transform's scale is uniform enough to compose exactly.
[[nodiscard]] bool HasUniformScale(const Transform &transform);

/// @brief @p world's rotation alone, with scale divided out of each basis vector.
///
/// Exact for uniform scale, which is what a blueprint instance root is
/// constrained to — and a non-uniformly
/// scaled matrix has no exact rotation to extract in the first place, because the
/// composition is a shear rather than a TRS.
[[nodiscard]] inline glm::quat WorldRotationOf(const glm::mat4 &world)
{
    const glm::mat3 basis(world);
    return glm::quat_cast(glm::mat3(glm::normalize(basis[0]), glm::normalize(basis[1]), glm::normalize(basis[2])));
}

/// @brief @p local placed into world space under @p parentWorld.
///
/// Position and rotation only: @p local's scale is carried through untouched
/// rather than composed, because the callers place rigid bodies, whose collider
/// dimensions do not scale with the entity.
[[nodiscard]] inline Transform PoseUnderParent(const Transform &local, const glm::mat4 &parentWorld)
{
    Transform out;
    out.position = glm::vec3(parentWorld * glm::vec4(local.position, 1.f));
    out.rotation = glm::normalize(WorldRotationOf(parentWorld) * local.rotation);
    out.scale = local.scale;
    return out;
}

} // namespace Assisi::ECS
