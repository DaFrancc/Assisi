/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ParentFrame.hpp
/// @brief A frame inside an entity's parent that its Transform is measured
///        from, in place of the parent's origin.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Prelude.hpp>

namespace Assisi::ECS
{

/// @brief Where inside its parent a child hangs: a pose in the parent's own
///        space, between the parent's world matrix and the child's Transform.
///
/// A child with one is placed at its parent's world matrix, then this frame,
/// then its own Transform, so the Transform is an offset from the frame. For
/// something held by a joint of an animated parent: the engine writes the
/// joint's pose here every frame, and the held thing's Transform stays the
/// offset its author gave it.
///
/// Transient: worked out on every machine, never saved or sent. Tracked, so
/// writing it places the child again at the next propagation. Write it only
/// when it changes, so a still frame costs nothing to place.
ACOMP(transient, tracked)
struct ParentFrame
{
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 position{0.f};
    glm::vec3 scale{1.f};
};

} // namespace Assisi::ECS
