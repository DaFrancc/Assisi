/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/StepWorld.hpp>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Chiara/Profile.hpp>
#include <Assisi/Core/Assert.hpp>
#include <Assisi/ECS/Hierarchy.hpp>

namespace Assisi::App
{

void StepWorld(const SystemContext &ctx, std::span<const ECS::Entity> replayedCharacters,
               std::span<const Physics::FollowerStep> followers, std::span<const ECS::Entity> replayedBodies)
{
    ASSISI_ASSERT(ctx.replaying || (replayedCharacters.empty() && followers.empty() && replayedBodies.empty()),
                  "StepWorld: a live step steps every character and every body");
    World &world = ctx.world;

    // Ask for what should happen, simulate it, react to what did. The phase
    // decides which side of the physics step a system lands on; ordering
    // within a phase cannot substitute for it. Everything inside the scope is
    // one fixed step to the render blend.
    const ECS::FixedStepScope step(world.scene);

    // A live tick reads what was pushed since the world's previous one ended,
    // so no tick reads an event another already has. A replay steps ticks that
    // ran live and read theirs then, so it moves no mark, and the messages that
    // have arrived wait for the live tick, which reads them once.
    if (!ctx.replaying)
    {
        ctx.events.BeginTick(world.eventTicks);
        world.net.Deliver(world.events);
    }
    world.systems.Run(SystemPhase::FixedUpdate, ctx);

    {
        // Jolt's whole step, including its internal job dispatch: everything
        // under `fixed-update` that is not a named ECS system.
        ASSISI_PROFILE_SCOPE("physics-step");
        if (ctx.replaying)
        {
            world.physics.StepCharacters(replayedCharacters, followers, ctx.dt, replayedBodies);
        }
        else
        {
            world.physics.Update(ctx.dt);
        }
    }

    world.systems.Run(SystemPhase::PostFixedUpdate, ctx);
    if (!ctx.replaying)
    {
        ctx.events.EndTick(world.eventTicks);
    }
}

} // namespace Assisi::App
