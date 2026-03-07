#pragma once

/// @file Hierarchy.hpp
/// @brief Parent-child entity relationships and world-space transform propagation.
///
/// Hierarchy is opt-in: add ParentComponent to a child entity to attach it to a
/// parent. The parent needs no modification.
///
/// TransformComponent stores local-space TRS. PropagateTransforms() walks the
/// parent chain and writes the result into TransformComponent::worldMatrix.
/// For root entities (no parent), worldMatrix == local TRS matrix.

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Prelude.hpp>

namespace Assisi::Runtime
{

/// @brief Marks an entity as a child of another entity.
///
/// The parent entity must have a TransformComponent. Entities without this
/// component are treated as roots (worldMatrix == local TRS matrix).
ACOMP()
struct ParentComponent
{
    AFIELD() ECS::Entity parent = ECS::NullEntity;
};

/// @brief Compute and cache world-space matrices for all entities with a TransformComponent.
///
/// Writes results into TransformComponent::worldMatrix. Must be called once per
/// frame before DrawScene() or any system that reads worldMatrix. Parent matrices
/// are memoised so each entity's chain is traversed at most once.
void PropagateTransforms(ECS::Scene &scene);

} // namespace Assisi::Runtime