/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Transform.hpp
/// @brief Local-space TRS component with a cached world matrix.
///
/// Transform is the engine's most foundational component: rendering, physics,
/// and the scene-graph hierarchy all read and write it. It lives here, in the
/// ECS layer, precisely so it stays free of any renderer dependency — lower
/// modules (e.g. Physics) can use it without pulling in nvrhi/GPU headers.
/// Runtime re-exports it from Components.hpp, so `Runtime::Transform` still
/// names this exact type for the render-facing code that grew up around it.

#include <Assisi/Prelude.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::ECS
{

/// @brief Local-space TRS with a cached world matrix updated by PropagateTransforms().
///
/// Write to position/rotation/scale to move an entity. Read worldMatrix for
/// rendering or any system that needs actual world-space coordinates.
/// worldMatrix is not serialized — it is recomputed every frame.
ACOMP()
struct Transform
{
    AFIELD() glm::vec3 position{0.f, 0.f, 0.f};
    AFIELD() glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    AFIELD() glm::vec3 scale{1.f, 1.f, 1.f};

    glm::mat4 worldMatrix{1.f}; ///< Computed by PropagateTransforms(). Do not set manually.
};

} // namespace Assisi::ECS
