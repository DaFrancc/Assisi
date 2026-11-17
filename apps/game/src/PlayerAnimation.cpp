/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "PlayerAnimation.hpp"

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/AnimatorParams.hpp>
#include <Assisi/Runtime/Components.hpp>

namespace Game
{

namespace
{

/// How long speed takes to close about two thirds of the way to the
/// character's (s): enough to smooth a correction's jolt, too little to see
/// the blend lag behind the feet.
constexpr float kSpeedEase = 0.1f;

/// How long a character is off the ground before it counts as airborne (s):
/// longer than a step down a stair, and as long as its physics waits before
/// counting a landing, so every fall ends in one.
constexpr float kStairFlicker = 0.1f;

} // namespace

void PlayerAnimationSystem(Assisi::App::SystemContext &ctx)
{
    using namespace Assisi;
    static Runtime::AnimatorParam<float> speed{"speed"};
    static Runtime::AnimatorParam<bool> airborne{"airborne"};
    static Runtime::AnimatorParam<bool> crouched{"crouched"};
    static Runtime::AnimatorTrigger jump{"jump"};
    static Runtime::AnimatorTrigger land{"land"};

    // A host with no window evaluates no animation.
    if (!ctx.animates)
    {
        return;
    }
    ECS::Scene &scene = ctx.world.scene;
    // The body is a child of the character, which carries the state.
    for (auto [body, run, parent] : scene.Query<Mut<Runtime::AnimatorRun>, ECS::Parent>())
    {
        if (!scene.Has<App::Player>(parent.parent))
        {
            continue;
        }
        const Physics::CharacterState *state = scene.Get<Physics::CharacterState>(parent.parent);
        if (state == nullptr)
        {
            continue;
        }
        speed.SetDamped(body, run, state->groundSpeed, kSpeedEase, ctx.dt);
        airborne.Set(body, run,
                     state->ground != Physics::GroundState::OnGround && state->timeSinceGrounded >= kStairFlicker);
        crouched.Set(body, run, state->stance == Physics::Stance::Crouching);
        jump.FireOnChange(body, run, state->jumps);
        land.FireOnChange(body, run, state->landings);
    }
}

void PlayGesture(Assisi::ECS::Scene &scene, Assisi::ECS::Entity player)
{
    using namespace Assisi;
    static Runtime::AnimatorTrigger interact{"interact"};
    for (auto [body, run, parent] : scene.Query<Mut<Runtime::AnimatorRun>, ECS::Parent>())
    {
        if (parent.parent == player)
        {
            interact.Fire(body, run);
        }
    }
}

} // namespace Game
