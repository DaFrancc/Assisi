/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ColliderPose.hpp
/// @brief Where a rigid body's collider actually is, for the editor's overlays.

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Editor
{

/// @brief The model matrix a collider overlay for @p entity draws at: the world
/// pose the physics world places its body at.
///
/// Rotation and translation only. A collider's scale is the shape's, which for a
/// round shape is not the Transform's, so the caller composes in what
/// PhysicsWorld::GetColliderScale reports. A parented entity's Transform is an
/// offset from its parent, so the parent's world matrix is composed in exactly as
/// Physics does, through ECS::ParentWorldMatrix; for an unparented one @p local
/// already is the world pose.
///
/// Reads the *parent's* propagated Transform::worldMatrix, so propagation must
/// have run this frame. Not the entity's own world matrix, whose scale a
/// non-uniformly scaled parent would shear into the rotation.
[[nodiscard]] glm::mat4 ColliderBodyModel(const ECS::Scene &scene, ECS::Entity entity, const ECS::Transform &local);

} // namespace Assisi::Editor
