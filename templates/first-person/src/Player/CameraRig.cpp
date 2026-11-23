/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file CameraRig.cpp
/// @brief The first-person template's camera rig. CameraRig.hpp says what it
///        does, how to drop it in and what to change.

#include "Player/CameraRig.hpp"

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>

namespace Game
{

void EyeHeightSystem(Assisi::App::SystemContext &ctx)
{
    Assisi::ECS::Scene &scene = ctx.world.scene;
    for (auto [character, state, rig] : scene.Query<Assisi::Physics::CharacterState, ViewRig>())
    {
        (void)character;
        const Assisi::ECS::Transform *current = scene.Get<Assisi::ECS::Transform>(rig.camera);
        const double eyeHeight = static_cast<double>(state.eyeHeight);
        // Compared first: a write marks the Transform changed, and a character
        // standing still would otherwise move its camera's children every step.
        if (current == nullptr || current->position.y == eyeHeight)
        {
            continue;
        }
        scene.GetMut<Assisi::ECS::Transform>(rig.camera)->position.y = eyeHeight;
    }
}

void PlayerViewSystem(Assisi::App::SystemContext &ctx)
{
    Assisi::ECS::Scene &scene = ctx.world.scene;
    for (auto [character, rig] : scene.Query<ViewRig>())
    {
        const Assisi::Runtime::Camera *camera = scene.Get<Assisi::Runtime::Camera>(rig.camera);
        const bool active = scene.Has<Assisi::App::LocalPlayer>(character);
        // Compared first, for the same reason as the eye height.
        if (camera == nullptr || camera->isActive == active)
        {
            continue;
        }
        scene.GetMut<Assisi::Runtime::Camera>(rig.camera)->isActive = active;
    }
}

} // namespace Game
