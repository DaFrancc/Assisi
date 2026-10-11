/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/PhysicsSystems.hpp>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>

namespace Assisi::App
{

void CharacterEyeSystem(SystemContext &ctx)
{
    ECS::Scene &scene = ctx.world.scene;
    for (auto [entity, state] : scene.Query<Physics::CharacterState>())
    {
        const float eyeHeight = state.eyeHeight;

        for (auto [child, camera, parent] : scene.Query<Runtime::Camera, ECS::Parent>())
        {
            (void)camera;
            if (parent.parent != entity)
            {
                continue;
            }

            // Compared first: a write stamps the Transform as changed, and a
            // character standing still would otherwise re-propagate its camera
            // every step.
            const ECS::Transform *current = scene.Get<ECS::Transform>(child);
            if (current == nullptr || current->position.y == eyeHeight)
            {
                continue;
            }
            scene.GetMut<ECS::Transform>(child)->position.y = eyeHeight;
        }
    }
}

} // namespace Assisi::App
