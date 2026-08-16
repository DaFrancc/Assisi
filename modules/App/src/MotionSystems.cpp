/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/MotionSystems.hpp>

#include <cmath>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Runtime/Components.hpp>

namespace Assisi::App
{

void OscillateSystem(SystemContext &ctx)
{
    constexpr float kTau = 6.28318530717958647692f;

    // The fixed-step tick is the clock, and dt is that step's length, so this is
    // simulated time rather than wall-clock time: identical in every run, and
    // unaffected by whatever the frame rate did.
    const float elapsed = static_cast<float>(ctx.simTick) * ctx.dt;

    ECS::Scene &scene = ctx.world.scene;
    for (auto [entity, oscillator] : scene.Query<Runtime::Oscillator>())
    {
        if (oscillator.periodSeconds <= 0.f)
        {
            continue; // parked without removing the component
        }

        const float axisLength = glm::length(oscillator.axis);
        if (axisLength <= 0.f)
        {
            continue;
        }

        const float offset =
            oscillator.amplitude * std::sin(kTau * (elapsed / oscillator.periodSeconds + oscillator.phase));

        // Transform is ACOMP(tracked) and the query hands out an unstamped
        // reference, so the write goes through GetMut or PropagateTransforms
        // never learns the pose moved.
        if (ECS::Transform *transform = scene.GetMut<ECS::Transform>(entity))
        {
            transform->position = oscillator.origin + (oscillator.axis / axisLength) * offset;
        }
    }
}

} // namespace Assisi::App
