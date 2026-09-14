/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/PhysicsSystems.hpp>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Window/ActionMap.hpp>
#include <Assisi/Window/InputContext.hpp>

#include <cmath>

#include <glm/geometric.hpp>

#include <algorithm>
#include <vector>

namespace Assisi::App
{

void BounceSystem(SystemContext &ctx)
{
    ECS::Scene &scene = ctx.world.scene;

    // Walk the events rather than querying for Bounce entities: contacts number in
    // the handful even when bouncers number in the thousands, and an entity that
    // touched nothing this step has nothing to do either way.
    const std::span<const Physics::ContactEvent> events = ctx.world.physics.ContactEvents();
    if (events.empty())
        return;

    // One bounce per entity per step (see the header). Function-local, so nothing
    // carries between frames or between the worlds this same function serves —
    // a system installed into several worlds must keep its state in components,
    // never in the system itself.
    std::vector<ECS::Entity> bounced;

    for (const Physics::ContactEvent &contact : events)
    {
        // Only the step a body arrives on. A resting body reports Stay forever,
        // and reflecting that would launch something that is simply lying there.
        // A sensor is skipped because it resisted nothing: a body passes through
        // it, so there is no surface to bounce off.
        if (contact.phase != Physics::ContactPhase::Enter || contact.sensor)
            continue;

        const Physics::Bounce *bounce = scene.Get<Physics::Bounce>(contact.entity);
        if (bounce == nullptr)
            continue;

        // The impact was logged against a live body during the last step; the
        // entity can still have been destroyed since, or had its collider removed.
        if (!scene.IsAlive(contact.entity))
            continue;
        const Physics::RigidBody *body = scene.Get<Physics::RigidBody>(contact.entity);
        if (body == nullptr)
            continue;

        // Only a body approaching hard enough bounces. Two things are being
        // rejected here, and both matter:
        //
        //  - Anything not moving into the surface (closing speed >= 0). Jolt's
        //    speculative contacts fire a little before a real touch and also for
        //    bodies already moving apart; reflecting one of those would drive it
        //    back into the surface it just left.
        //  - Anything slower than kMinBounceSpeed. That is the floor that lets a
        //    bouncy body come to rest — see the constant for why settling noise
        //    otherwise feeds itself.
        //
        // The measure is the closing speed, not the speed: a body skimming fast
        // along a floor is barely touching it, and should not be launched for it.
        const float closingSpeed = glm::dot(contact.velocity, contact.normal);
        if (closingSpeed > -kMinBounceSpeed)
            continue;

        if (std::find(bounced.begin(), bounced.end(), contact.entity) != bounced.end())
            continue;
        bounced.push_back(contact.entity);

        // Mirror the incoming velocity about the surface, then scale it. Reflection
        // rather than plain negation is what makes it read as a bouncy ball: a body
        // arriving at an angle leaves at the mirrored angle and keeps its sideways
        // travel, instead of retracing the path it came in on. For a head-on hit
        // the two are the same thing.
        //
        // `rebound` is a speed multiplier, which is what makes 0 / 1 / >1 behave as
        // "stops dead" / "loses nothing" / "gains on every hit". Negative is
        // clamped rather than trusted: the inspector floors the field, but a level
        // file is just text and can hold anything.
        const glm::vec3 reflected = contact.velocity - 2.f * closingSpeed * contact.normal;
        const float rebound   = glm::max(bounce->rebound, 0.f);
        ctx.world.physics.SetBodyLinearVelocity(*body, reflected * rebound);
    }
}

void CharacterMoveSystem(SystemContext &ctx)
{
    ECS::Scene &scene = ctx.world.scene;

    for (auto [entity, character, descriptor] :
         scene.QueryMut<Physics::Character, Physics::CharacterDescriptor>())
    {
        (void)entity;

        const Physics::Character           &intent   = character.Get();
        const Physics::CharacterDescriptor &authored = descriptor.Get();

        const glm::vec3 move = intent.move;
        const bool      jump = intent.jump;

        // Asked every step rather than on the edge of a keypress: standing up
        // under something low fails, and retrying is what lets a character stand
        // by itself once it has walked clear.
        (void)ctx.world.physics.SetCharacterStance(intent, intent.stance);

        // The crouch scale applies to the stance the character actually reached,
        // not the one it asked for — read back live, because a character blocked
        // under a ledge would otherwise walk at full speed while crouched.
        const Physics::Stance stance = ctx.world.physics.GetCharacterState(intent).stance;
        const float           speed  = stance == Physics::Stance::Crouching
                                           ? authored.walkSpeed * authored.crouchSpeedScale
                                           : authored.walkSpeed;

        ctx.world.physics.MoveCharacter(intent, move * speed, jump);

        // A request, consumed: one press is one jump however many steps pass
        // before it can fire.
        character.GetMut().jump = false;
    }
}

void CharacterStateSystem(SystemContext &ctx)
{
    for (auto [entity, character] : ctx.world.scene.QueryMut<Physics::Character>())
    {
        (void)entity;

        const Physics::CharacterState state = ctx.world.physics.GetCharacterState(character.Get());

        // Skip the write when nothing moved, rather than stamp a change tick for
        // a state identical to the one already there. Character is transient and
        // untracked, so the tick costs nothing today — but a standing crowd of
        // characters would otherwise write every field every step for no reason.
        if (character.Get().state.velocity == state.velocity &&
            character.Get().state.ground == state.ground &&
            character.Get().state.stance == state.stance &&
            character.Get().state.groundEntity == state.groundEntity)
        {
            continue;
        }

        character.GetMut().state = state;
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

    for (auto [entity, character] : scene.QueryMut<Physics::Character>())
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
            glm::normalize(glm::angleAxis(glm::radians(yawDegrees), glm::vec3(0.f, 1.f, 0.f)) *
                           transform->rotation);

        // Pitch on the camera parented to this character, so the body turns and
        // the head tilts — a pitched capsule would walk into the floor.
        for (auto [child, camera, parent] : scene.Query<Runtime::Camera, Runtime::Parent>())
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

            childTransform->rotation =
                glm::normalize(glm::angleAxis(glm::radians(pitch), glm::vec3(1.f, 0.f, 0.f)));
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

    const Window::InputContext &input   = *ctx.input;
    const Window::ActionMap    &actions = *ctx.actions;

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
        actions.IsActionDown(kActionCrouch, input) ? Physics::Stance::Crouching
                                                   : Physics::Stance::Standing;

    for (auto [entity, character] : ctx.world.scene.QueryMut<Physics::Character>())
    {
        // Into the world, through the character's own facing. The vertical part is
        // dropped: looking down must not walk a character into the floor, and the
        // controller ignores it anyway.
        glm::vec3 worldMove = move;
        if (const ECS::Transform *transform = ctx.world.scene.Get<ECS::Transform>(entity))
        {
            worldMove   = transform->rotation * move;
            worldMove.y = 0.f;
            if (glm::dot(worldMove, worldMove) > 0.f)
            {
                worldMove = glm::normalize(worldMove);
            }
        }

        Physics::Character &intent = character.GetMut();
        intent.move   = worldMove;
        intent.stance = stance;

        // OR rather than assign: CharacterMoveSystem clears the request once it
        // has been consumed, and a press landing on a step this system happens to
        // run twice for must not be wiped before that.
        intent.jump = intent.jump || jump;
    }
}

} // namespace Assisi::App
