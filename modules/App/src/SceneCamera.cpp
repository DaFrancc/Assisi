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

        ECS::WorldMatrix world = *scene.Get<ECS::WorldMatrix>(entity);
        if (const Runtime::CameraDrawPose *drawn = scene.Get<Runtime::CameraDrawPose>(entity))
        {
            // Where it stands stays blended, so moving is as smooth as before.
            world.inCell = glm::translate(glm::mat4(1.f), glm::vec3(world.inCell[3]) + drawn->offset) *
                           glm::mat4_cast(drawn->rotation);
        }
        found = SceneView{.world = world, .pose = *transform, .camera = sceneCamera, .entity = entity};
    }

    if (found)
    {
        found->camera.farZ = Runtime::SceneDrawDistance(scene, found->camera.farZ);
    }
    return found;
}

} // namespace Assisi::App
