/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/PhysicsSystems.hpp>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Window/ActionMap.hpp>
#include <Assisi/Window/InputContext.hpp>

#include <cmath>

#include <glm/geometric.hpp>

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

void CharacterLookSystem(SystemContext &ctx)
{
    // Looks only while the cursor is held, and never takes it: whoever owns the
    // session owns the cursor, because only it knows when the player has asked for
    // the mouse back. Without that split, releasing would last exactly one step
    // before this grabbed it again.
    if (ctx.input == nullptr || !ctx.input->IsMouseCaptured())
    {
        return;
    }

    const glm::vec2 delta = ctx.input->MouseDelta();
    if (delta.x == 0.f && delta.y == 0.f)
    {
        return;
    }

    ECS::Scene &scene = ctx.world.scene;

    for (auto [entity, character] : scene.Query<Physics::Character>())
    {
        (void)character;

        // Yaw on the character, about world up rather than its own axis: composing
        // onto the existing rotation would let pitch and roll leak in and the
        // character would end up tipped over after enough looking around.
        ECS::Transform *transform = scene.GetMut<ECS::Transform>(entity);
        if (transform == nullptr)
        {
            continue;
        }

        const float yawDegrees = -delta.x * kLookDegreesPerPixel;
        transform->rotation =
            glm::normalize(glm::angleAxis(glm::radians(yawDegrees), glm::vec3(0.f, 1.f, 0.f)) * transform->rotation);

        // Pitch on the camera parented to this character, so the body turns and
        // the head tilts — a pitched capsule would walk into the floor.
        for (auto [child, camera, parent] : scene.Query<Runtime::Camera, ECS::Parent>())
        {
            (void)camera;
            if (parent.parent != entity)
            {
                continue;
            }

            ECS::Transform *childTransform = scene.GetMut<ECS::Transform>(child);
            if (childTransform == nullptr)
            {
                continue;
            }

            // Rebuilt from an accumulated angle rather than multiplied in, so the
            // clamp is a clamp: composing a delta each step and then limiting the
            // result lets it creep past the limit and stick there.
            const glm::vec3 forward = childTransform->rotation * glm::vec3(0.f, 0.f, -1.f);
            const float currentPitch = glm::degrees(std::asin(glm::clamp(forward.y, -1.f, 1.f)));
            const float pitch =
                glm::clamp(currentPitch - delta.y * kLookDegreesPerPixel, -kMaxPitchDegrees, kMaxPitchDegrees);

            childTransform->rotation = glm::normalize(glm::angleAxis(glm::radians(pitch), glm::vec3(1.f, 0.f, 0.f)));
        }
    }
}

void CharacterInputSystem(SystemContext &ctx)
{
    // Null on a headless host, which has no devices to poll. The system is
    // activeWorldOnly and so is gated out of one anyway; this is what makes it
    // inert rather than a crash if a level names it somewhere unexpected.
    if (ctx.input == nullptr || ctx.actions == nullptr)
    {
        return;
    }

    const Window::InputContext &input = *ctx.input;
    const Window::ActionMap &actions = *ctx.actions;

    // Built in the character's own frame — forward is −Z — and rotated into the
    // world per character below, so walking follows where each one is looking
    // rather than a fixed compass direction.
    glm::vec3 move{0.f};
    if (actions.IsActionDown(kActionMoveForward, input))
    {
        move.z -= 1.f;
    }
    if (actions.IsActionDown(kActionMoveBackward, input))
    {
        move.z += 1.f;
    }
    if (actions.IsActionDown(kActionMoveLeft, input))
    {
        move.x -= 1.f;
    }
    if (actions.IsActionDown(kActionMoveRight, input))
    {
        move.x += 1.f;
    }

    // Normalized, or holding two keys would walk faster diagonally than straight.
    // Guarded because normalizing a zero vector is a division by zero that poisons
    // every downstream number with NaN.
    if (glm::dot(move, move) > 0.f)
    {
        move = glm::normalize(move);
    }

    const bool jump = actions.IsActionPressed(kActionJump, input);
    const Physics::Stance stance =
        actions.IsActionDown(kActionCrouch, input) ? Physics::Stance::Crouching : Physics::Stance::Standing;

    for (auto [entity, intent] : ctx.world.scene.Query<Mut<Physics::CharacterIntent>>())
    {
        // Into the world, through the character's own facing. The vertical part is
        // dropped: looking down must not walk a character into the floor, and the
        // controller ignores it anyway.
        glm::vec3 worldMove = move;
        if (const ECS::Transform *transform = ctx.world.scene.Get<ECS::Transform>(entity))
        {
            worldMove = transform->rotation * move;
            worldMove.y = 0.f;
            if (glm::dot(worldMove, worldMove) > 0.f)
            {
                worldMove = glm::normalize(worldMove);
            }
        }

        intent.move = worldMove;
        intent.stance = stance;

        // OR rather than assign: the step clears the request once it has been
        // consumed, and a press must survive frames that run no step.
        intent.jump = intent.jump || jump;
    }
}

} // namespace Assisi::App
