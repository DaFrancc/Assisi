/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file DrawPose.hpp
/// @brief The rotation an entity is drawn with this frame, ahead of its
///        Transform, for whatever input turns between fixed steps.

#include <Assisi/Prelude.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::ECS
{

/// @brief How an entity is drawn this frame, in place of the rotation its
/// Transform gives it: for whatever turns every frame without turning what is
/// simulated, such as a camera's look that follows the mouse between fixed
/// steps, head bob or screen shake.
///
/// Set each frame by whatever turns it. PropagateTransforms applies it to the
/// world matrix after the blend between fixed steps: the entity is drawn at
/// `rotation` in the world, keeping its scale, where the blend puts it plus
/// `offset`, and its children are drawn under that pose, so a weapon held in
/// front of a camera turns with the view every frame. The Transform, what is
/// simulated and what is sent stay as they are. Applied before a RenderOffset.
/// Remove it to draw the entity as its Transform has it again.
ACOMP(transient)
struct DrawPose
{
    AFIELD() glm::quat rotation{1.f, 0.f, 0.f, 0.f}; ///< The rotation in the world it is drawn with.
    AFIELD() glm::vec3 offset{0.f};                  ///< Added to where it stands, in the world (m).
};

} // namespace Assisi::ECS
