/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/SceneCamera.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Runtime/SkyResolve.hpp>
#include <Assisi/Runtime/TextureCamera.hpp>

namespace Assisi::App
{

std::optional<SceneView> ActiveSceneCamera(ECS::Scene &scene)
{
    std::optional<SceneView> found;

    for (auto [entity, sceneCamera] : scene.Query<Runtime::Camera>())
    {
        // A texture camera draws into its picture, never to the screen.
        if (!sceneCamera.isActive || scene.Has<Runtime::TextureCamera>(entity))
        {
            continue;
        }

        // A Camera without a Transform has no position to look from. Skipped
        // rather than treated as the answer, so a half-built entity does not
        // shadow a complete camera later in the scene.
        const ECS::Transform *transform = scene.Get<ECS::Transform>(entity);
        if (transform == nullptr)
        {
            continue;
        }

        if (found)
        {
            Core::Log::Warn("Scene camera: more than one Camera is marked active; looking through "
                            "the first one found. Clear isActive on the others.");
            break;
        }

        // As drawn: a DrawPose is already in it, applied by the transform pass
        // so whatever hangs from the camera turns with it.
        found = SceneView{.world = *scene.Get<ECS::WorldMatrix>(entity),
                          .pose = *transform,
                          .camera = sceneCamera,
                          .entity = entity};
    }

    if (found)
    {
        found->camera.farZ = Runtime::SceneDrawDistance(scene, found->camera.farZ);
    }
    return found;
}

} // namespace Assisi::App
