/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/WorldFrame.hpp>

#include <chrono>

#include <Assisi/App/AudioSystems.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/Runtime/SkinnedMeshPose.hpp>

namespace Assisi::App
{

namespace
{

/// Seconds on a clock that only goes forward, for PoseTimes.
[[nodiscard]] double MonotonicSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

void BeginWorldFrame(const SystemContext &ctx)
{
    ctx.world.systems.Run(SystemPhase::PreUpdate, ctx);
    ctx.world.events.MarkTicks();
}

void UpdateWorld(const SystemContext &ctx)
{
    ctx.world.systems.Run(SystemPhase::Update, ctx);
}

void PoseWorld(World &world, Render::AssetCache *cache, PoseTimes *times)
{
    const double start = times != nullptr ? MonotonicSeconds() : 0.0;
    world.propagationTick = ECS::PropagateTransforms(world.scene, world.propagationTick);
    const double placed = times != nullptr ? MonotonicSeconds() : 0.0;
    Runtime::FinishPoses(world.scene, cache);
    Runtime::FollowJoints(world.scene);
    const double posed = times != nullptr ? MonotonicSeconds() : 0.0;
    world.propagationTick = ECS::PropagateTransforms(world.scene, world.propagationTick);
    world.posed = true;
    if (times != nullptr)
    {
        times->placing += placed - start;
        times->posing += posed - placed;
        times->placingAgain += MonotonicSeconds() - posed;
    }
}

void EnsurePosed(World &world, Render::AssetCache *cache)
{
    if (!world.posed)
    {
        PoseWorld(world, cache);
    }
}

void PresentWorld(const SystemContext &ctx)
{
    ctx.world.systems.Run(SystemPhase::Ui, ctx);
    PlayWorldSounds(ctx);
}

void EndWorldFrame(World &world, Core::EventQueue &universal, bool ticking)
{
    world.events.EndFrame(/*keepPending=*/ ticking);
    world.posed = false;

    // The universal queue keeps what waits for whichever worlds are ticking,
    // so one that is not lets it go by rather than read it when it resumes.
    if (!ticking)
    {
        universal.SkipPending(world.eventTicks.universal);
    }
}

} // namespace Assisi::App
