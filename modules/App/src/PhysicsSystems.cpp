/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/PhysicsSystems.hpp>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Window/ActionMap.hpp>
#include <Assisi/Window/InputContext.hpp>

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

    // World axes, because there is no camera to be relative to yet: forward is
    // −Z. Possession is what replaces this whole system.
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
        (void)entity;
        Physics::Character &intent = character.GetMut();
        intent.move   = move;
        intent.stance = stance;

        // OR rather than assign: CharacterMoveSystem clears the request once it
        // has been consumed, and a press landing on a step this system happens to
        // run twice for must not be wiped before that.
        intent.jump = intent.jump || jump;
    }
}

} // namespace Assisi::App
