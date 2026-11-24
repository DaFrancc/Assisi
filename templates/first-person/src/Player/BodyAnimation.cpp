/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file BodyAnimation.cpp
/// @brief The first-person template's body animation. BodyAnimation.hpp says
///        what it does, how to drop it in and what to change; add a param for
///        your animator here.

#include "Player/BodyAnimation.hpp"

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

void BodyAnimationSystem(Assisi::App::SystemContext &ctx)
{
    using Assisi::Physics::CharacterState;
    using Assisi::Physics::GroundState;
    using Assisi::Physics::Stance;
    using Assisi::Runtime::AnimatorParam;
    using Assisi::Runtime::AnimatorTrigger;

    static AnimatorParam<float> speed{"speed"};
    static AnimatorParam<bool> airborne{"airborne"};
    static AnimatorParam<bool> crouched{"crouched"};
    static AnimatorTrigger jump{"jump"};
    static AnimatorTrigger land{"land"};

    // A host with no window evaluates no animation.
    if (!ctx.animates)
    {
        return;
    }
    Assisi::ECS::Scene &scene = ctx.world.scene;
    for (auto [body, run, parent] :
         scene.Query<Assisi::ECS::Mut<Assisi::Runtime::AnimatorRun>, Assisi::ECS::Parent>())
    {
        const CharacterState *state = scene.Get<CharacterState>(parent.parent);
        if (state == nullptr)
        {
            continue;
        }
        speed.SetDamped(body, run, state->groundSpeed, kSpeedEase, ctx.dt);
        airborne.Set(body, run, state->ground != GroundState::OnGround && state->timeSinceGrounded >= kStairFlicker);
        crouched.Set(body, run, state->stance == Stance::Crouching);
        jump.FireOnChange(body, run, state->jumps);
        land.FireOnChange(body, run, state->landings);
    }
}

void PlayGesture(Assisi::ECS::Scene &scene, Assisi::ECS::Entity character)
{
    static Assisi::Runtime::AnimatorTrigger interact{"interact"};
    for (auto [body, run, parent] :
         scene.Query<Assisi::ECS::Mut<Assisi::Runtime::AnimatorRun>, Assisi::ECS::Parent>())
    {
        if (parent.parent == character)
        {
            interact.Fire(body, run);
        }
    }
}

} // namespace Game
