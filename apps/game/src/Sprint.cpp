/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "Sprint.hpp"

#include <Assisi/App/World.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

namespace Game
{

void SprintSystem(Assisi::App::SystemContext &ctx)
{
    for (auto [entity, controls, intent] :
         ctx.world.scene.Query<Assisi::App::PlayerInput, Mut<Assisi::Physics::CharacterIntent>>())
    {
        (void)entity;
        intent.speedScale = controls.held.Has(Sprint) ? kSprintSpeedScale : 1.f;
    }
}

} // namespace Game
