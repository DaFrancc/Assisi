/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WorldMatrix.hpp
/// @brief The world-space matrix an entity is drawn with.

#include <Assisi/Prelude.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::ECS
{

/// @brief Where an entity is drawn, in world space.
///
/// Every Transform brings one. PropagateTransforms writes it: the local pose
/// composed with the parents', blended between fixed steps, with any
/// RenderOffset applied. Read it to draw; read the Transform to simulate, since
/// between steps the two differ. Writing it does nothing lasting — the next
/// propagation that touches the entity overwrites it.
///
/// Tracked: a propagation that moves it on screen stamps its change tick, so
/// ChangedSince<WorldMatrix> is everything whose drawn pose changed — a body
/// blending between two fixed steps included, though its Transform is written
/// only on step frames.
///
/// Identity until the first propagation after it is added. Removing only the
/// Transform leaves it until that propagation, which removes it too.
ACOMP(transient, tracked)
struct WorldMatrix
{
    glm::mat4 matrix{1.f};
};

} // namespace Assisi::ECS
