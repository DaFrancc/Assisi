/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "AnimatorKeys.hpp"

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/AnimatorParams.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

namespace Game
{

namespace
{

/// Speeds in the units the locomotion blend space is laid out in: walking,
/// and running with Shift held.
constexpr float kWalkSpeed = 1.5f;
constexpr float kRunSpeed = 6.f;

/// How long speed takes to close about two thirds of the way to the one the
/// keys ask for (s), so the blend moves through jog rather than jumping to it.
constexpr float kSpeedEase = 0.15f;

/// ...and aiming, between 0 and 1 (s).
constexpr float kAimEase = 0.1f;

} // namespace

void AnimatorKeysSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.input == nullptr) // headless host: no devices to read
    {
        return;
    }
    using Assisi::Window::Key;
    const Assisi::Window::InputContext &input = *ctx.input;
    const bool walking = input.IsKeyDown(Key::W);
    const float targetSpeed = !walking ? 0.f : (input.IsKeyDown(Key::LeftShift) ? kRunSpeed : kWalkSpeed);
    const float targetAim = input.IsMouseButtonDown(Assisi::Window::MouseButton::Right) ? 1.f : 0.f;
    // Grounded on the frame Space goes down, so the jump can start, and in the
    // air for as long as it stays held.
    const bool grounded = !input.IsKeyDown(Key::Space) || input.IsKeyPressed(Key::Space);

    static Assisi::Runtime::AnimatorParam<float> speed{"speed"};
    static Assisi::Runtime::AnimatorParam<float> aiming{"aiming"};
    static Assisi::Runtime::AnimatorParam<bool> groundedParam{"grounded"};
    static Assisi::Runtime::AnimatorTrigger jump{"jump"};

    // The params live in each Animator's run, which is never sent. A player's
    // body is PlayerAnimation's, which follows what the character does.
    Assisi::ECS::Scene &scene = ctx.world.scene;
    for (auto [entity, run] : scene.Query<Mut<Assisi::Runtime::AnimatorRun>>())
    {
        if (const Assisi::ECS::Parent *parent = scene.Get<Assisi::ECS::Parent>(entity);
            parent != nullptr && scene.Has<Assisi::App::Player>(parent->parent))
        {
            continue;
        }
        (void)speed.SetDamped(entity, run, targetSpeed, kSpeedEase, ctx.dt);
        (void)aiming.SetDamped(entity, run, targetAim, kAimEase, ctx.dt);
        (void)groundedParam.Set(entity, run, grounded);
        if (input.IsKeyPressed(Key::Space))
        {
            (void)jump.Fire(entity, run);
        }
    }
}

} // namespace Game
