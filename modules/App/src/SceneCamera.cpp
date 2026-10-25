/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/SceneCamera.hpp>

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/PlayerSystems.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>

namespace Assisi::App
{

namespace
{

/// Where a camera on this machine's player looks: the facing the last step
/// left its character and it at, turned and tilted on by what the player has
/// turned since and no step has used yet. Unblended, so aim follows the mouse
/// every frame rather than at the step's pace. Empty for any other camera.
///
/// A player's character is a root, moved in world space, so its Transform is
/// its world rotation.
std::optional<glm::quat> PlayerLook(const ECS::Scene &scene, ECS::Entity camera, const ECS::Transform &eye)
{
    const ECS::Parent *parent = scene.Get<ECS::Parent>(camera);
    if (parent == nullptr || !scene.Has<LocalPlayer>(parent->parent))
    {
        return std::nullopt;
    }
    const PlayerInput *controls = scene.Get<PlayerInput>(parent->parent);
    const ECS::Transform *body = scene.Get<ECS::Transform>(parent->parent);
    if (controls == nullptr || body == nullptr)
    {
        return std::nullopt;
    }
    return glm::normalize(Turned(body->rotation, controls->lookYaw) * Tilted(eye.rotation, controls->lookPitch));
}

} // namespace

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

        glm::mat4 world = scene.Get<ECS::WorldMatrix>(entity)->matrix;
        if (const std::optional<glm::quat> look = PlayerLook(scene, entity, *transform))
        {
            // Where it stands stays blended, so moving is as smooth as before.
            world = glm::translate(glm::mat4(1.f), glm::vec3(world[3])) * glm::mat4_cast(*look);
        }
        found = SceneView{.world = world, .pose = *transform, .camera = sceneCamera, .entity = entity};
    }

    return found;
}

} // namespace Assisi::App
