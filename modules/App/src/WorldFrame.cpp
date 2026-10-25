/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/WorldFrame.hpp>

#include <Assisi/App/AudioSystems.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/EventQueue.hpp>

namespace Assisi::App
{

void BeginWorldFrame(const SystemContext &ctx)
{
    ctx.world.systems.Run(SystemPhase::PreUpdate, ctx);
    ctx.world.events.MarkTicks();
}

void UpdateWorld(const SystemContext &ctx)
{
    ctx.world.systems.Run(SystemPhase::Update, ctx);
    ctx.world.systems.Run(SystemPhase::Ui, ctx);
    PlayWorldSounds(ctx);
}

void EndWorldFrame(World &world, Core::EventQueue &universal, bool ticking)
{
    world.events.EndFrame(/*keepPending=*/ ticking);

    // The universal queue keeps what waits for whichever worlds are ticking,
    // so one that is not lets it go by rather than read it when it resumes.
    if (!ticking)
    {
        universal.SkipPending(world.eventTicks.universal);
    }
}

} // namespace Assisi::App
