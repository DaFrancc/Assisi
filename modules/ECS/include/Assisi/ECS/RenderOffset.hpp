/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file RenderOffset.hpp
/// @brief A world-space offset to where an entity is drawn, leaving its
///        Transform alone.

#include <Assisi/Prelude.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::ECS
{

/// @brief Moves where an entity is drawn without moving the entity.
///
/// PropagateTransforms applies it to the world matrix after the render blend:
/// the rotation turns the entity about its own world position, then the
/// position is added. Children are drawn under the offset pose. The Transform,
/// which is the simulation pose, is never changed by it.
///
/// For hiding a correction: replication snaps a mirror to the server's pose and
/// lets this carry the difference away over a few frames.
ACOMP(transient)
struct RenderOffset
{
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 position{0.f};
};

} // namespace Assisi::ECS
