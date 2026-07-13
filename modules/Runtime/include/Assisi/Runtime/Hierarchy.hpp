/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Hierarchy.hpp
/// @brief Parent-child entity relationships and world-space transform propagation.
///
/// Hierarchy is opt-in: add Parent to a child entity to attach it to a
/// parent. The parent needs no modification.
///
/// Transform stores local-space TRS. PropagateTransforms() walks the
/// parent chain and writes the result into Transform::worldMatrix.
/// For root entities (no parent), worldMatrix == local TRS matrix.

#include <cstdint>

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Prelude.hpp>

namespace Assisi::Runtime
{

/// @brief Marks an entity as a child of another entity.
///
/// The parent entity must have a Transform. Entities without this
/// component are treated as roots (worldMatrix == local TRS matrix).
ACOMP()
struct Parent
{
    AFIELD() ECS::Entity parent = ECS::NullEntity;
};

/// @brief Refresh cached world-space matrices for entities whose transform changed.
///
/// Writes results into Transform::worldMatrix. Must be called once per frame before
/// DrawScene() or any system that reads worldMatrix. Transform is ACOMP(tracked),
/// so this only recomputes entities whose local TRS changed since `lastTick` (or
/// whose ancestor changed) — a static scene costs almost nothing. Parent chains are
/// resolved parent-before-child and each entity is visited at most once per pass.
///
/// @param lastTick The scene change tick this system last ran at (pass 0 on the
///        first call, which recomputes everything).
/// @return The scene's current change tick — pass it back as `lastTick` next frame
///         to skip unchanged entities. Discarding it is safe (you simply lose the
///         skip, recomputing everything if you keep passing 0).
uint64_t PropagateTransforms(ECS::Scene &scene, uint64_t lastTick);

} // namespace Assisi::Runtime