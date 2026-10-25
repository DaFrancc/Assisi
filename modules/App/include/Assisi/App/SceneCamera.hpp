/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SceneCamera.hpp
/// @brief The scene's own point of view — which camera a world is looked at
///        through when nothing outside the scene overrides it.
///
/// A game has only this one; an editor has this and its own fly camera, and
/// chooses between them by play state. The choosing is the caller's, so it is
/// not here: what is here is the single answer to "which Camera does this scene
/// nominate", so the two hosts cannot disagree about it.

#include <Assisi/ECS/SceneFwd.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <optional>

namespace Assisi::App
{

/// @brief A camera and the pose it looks from, as a pair that travels together.
struct SceneView
{
    /// Where the camera is, as propagated; what Runtime::ViewMatrix takes.
    glm::mat4 world{1.f};

    /// The camera's own Transform, which is relative to its parent if it has one.
    Runtime::Transform pose;

    Runtime::Camera camera;

    /// The camera's entity, which says whose eyes these are: a mesh set
    /// MeshRenderer::hiddenFromOwnCamera is not drawn through a camera of its own
    /// blueprint instance.
    ECS::Entity entity = ECS::NullEntity;
};

/// @brief The first active Camera in @p scene, with the Transform it sits at.
///
/// Empty when the scene nominates none, which is a caller's decision to make
/// rather than an error: a game falls back to a default view, an editor to the
/// camera the author is flying.
///
/// A camera on this machine's player looks where the player has turned so far,
/// every frame, including turning no fixed step has used yet, and is not blended
/// between steps; it still stands where the blend puts it. Aim then follows the
/// mouse at the frame rate rather than at the step's, while the step keeps the
/// real aim. Every other camera is drawn as propagated.
///
/// Two active cameras warn and the first found wins. Which one that is depends
/// on entity order, so it is arbitrary — said out loud rather than picked in
/// silence, because the symptom otherwise is a view through a camera nobody
/// meant, which reads as the intended one being broken.
[[nodiscard]] std::optional<SceneView> ActiveSceneCamera(ECS::Scene &scene);

} // namespace Assisi::App
