/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/SceneCamera.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Transform.hpp>

namespace Assisi::App
{

std::optional<SceneView> ActiveSceneCamera(ECS::Scene &scene)
{
    std::optional<SceneView> found;

    for (auto [entity, sceneCamera] : scene.Query<Runtime::Camera>())
    {
        if (!sceneCamera.isActive)
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

        found = SceneView{.pose = *transform, .camera = sceneCamera};
    }

    return found;
}

} // namespace Assisi::App
