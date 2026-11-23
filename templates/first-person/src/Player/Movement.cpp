/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file Movement.cpp
/// @brief The first-person template's movement. Movement.hpp says what it does,
///        how to drop it in and what to change; change what each button asks
///        for here.

#include "Player/Movement.hpp"

#include "Player/PlayerActions.hpp"

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

namespace Game
{

namespace
{

/// @p move, in the character's frame (x right, y forward), as a direction in
/// the world along the ground, of the same length: looking down must not walk
/// a character into the floor.
glm::vec3 WorldMove(const glm::quat &facing, glm::vec2 move)
{
    const float length = glm::min(glm::length(move), 1.f);
    glm::vec3 world = facing * glm::vec3{move.x, 0.f, -move.y};
    world.y = 0.f;
    if (length <= 0.f || glm::dot(world, world) <= 0.f)
    {
        return glm::vec3{0.f};
    }
    return glm::normalize(world) * length;
}

} // namespace

void MovementSystem(Assisi::App::SystemContext &ctx)
{
    using Assisi::App::PlayerAction;
    using Assisi::App::PlayerInput;
    using Assisi::Physics::CharacterIntent;
    using Assisi::Physics::Stance;

    Assisi::ECS::Scene &scene = ctx.world.scene;
    for (auto [character, controls, movement, intent] :
         scene.Query<PlayerInput, Movement, Assisi::ECS::Mut<CharacterIntent>>())
    {
        const Assisi::ECS::Transform *transform = scene.Get<Assisi::ECS::Transform>(character);
        const glm::quat facing = transform != nullptr ? transform->rotation : glm::quat{1.f, 0.f, 0.f, 0.f};
        const float scale = controls.held.Has(Actions::Walk) ? movement.walkMoveScale : 1.f;

        intent.move = WorldMove(facing, controls.move * scale);
        intent.stance = controls.held.Has(PlayerAction::Crouch) ? Stance::Crouching : Stance::Standing;
        intent.sprint = controls.held.Has(PlayerAction::Sprint);
        // OR rather than assign: the controller clears the request once it has
        // jumped, and a press must survive steps that could not jump yet.
        intent.jump = intent.jump || controls.pressed.Has(PlayerAction::Jump);
    }
}

} // namespace Game
