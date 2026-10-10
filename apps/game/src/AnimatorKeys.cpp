/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "AnimatorKeys.hpp"

#include <Assisi/App/World.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/AnimatorStep.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

#include <algorithm>
#include <optional>

namespace Game
{

namespace
{

/// Speeds in the units the locomotion blend space is laid out in: walking,
/// and running with Shift held.
constexpr float kWalkSpeed = 1.5f;
constexpr float kRunSpeed = 6.f;

/// How fast speed eases towards the one the keys ask for, in its units a
/// second, so the blend moves through jog rather than jumping to it.
constexpr float kSpeedRate = 8.f;

/// How fast aiming eases between 0 and 1, in units a second.
constexpr float kAimRate = 5.f;

/// @p current moved towards @p target by at most @p step.
float Towards(float current, float target, float step)
{
    return current < target ? std::min(current + step, target) : std::max(current - step, target);
}

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

    for (auto [entity, animator] : ctx.world.scene.Query<Mut<Assisi::Runtime::Animator>>())
    {
        const float speed = Assisi::Runtime::AnimatorFloat(animator, "speed").value_or(0.f);
        const float aiming = Assisi::Runtime::AnimatorFloat(animator, "aiming").value_or(0.f);
        (void)Assisi::Runtime::SetAnimatorFloat(animator, "speed", Towards(speed, targetSpeed, kSpeedRate * ctx.dt));
        (void)Assisi::Runtime::SetAnimatorFloat(animator, "aiming", Towards(aiming, targetAim, kAimRate * ctx.dt));
        (void)Assisi::Runtime::SetAnimatorBool(animator, "grounded", grounded);
        if (input.IsKeyPressed(Key::Space))
        {
            (void)Assisi::Runtime::FireAnimatorTrigger(animator, "jump");
        }
    }
}

} // namespace Game
