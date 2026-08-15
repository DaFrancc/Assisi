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
/// pose PhysicsWorld::AddBodyFromDescriptor created the body at.
///
/// Rotation and translation only, no scale — that is how the Jolt body was built,
/// and the descriptor's dimensions are absolute world units, so the overlay
/// traces the body rather than the mesh. A parented entity's Transform is an
/// offset from its parent, so the parent's world matrix is composed in exactly as
/// App::ParentWorldResolver hands it to Physics; for an unparented one @p local
/// already is the world pose.
///
/// Reads the *parent's* propagated Transform::worldMatrix, so propagation must
/// have run this frame. Not the entity's own world matrix, which carries its
/// local scale: physics ignores that scale, and a non-uniformly scaled body would
/// shear the rotation out of it.
[[nodiscard]] glm::mat4 ColliderBodyModel(const ECS::Scene &scene, ECS::Entity entity,
                                          const ECS::Transform &local);

} // namespace Assisi::Editor
