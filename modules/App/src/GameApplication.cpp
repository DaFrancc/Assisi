/// @file GameApplication.cpp

#include <Assisi/App/GameApplication.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/Lifecycle.hpp>

namespace Assisi::App
{

void GameApplication::OnStart()
{
    // --- Default FixedUpdate systems ---

    _systems.Register(SystemPhase::FixedUpdate, "PhysicsStep",
                      [this](SystemContext &ctx) { _physics.Update(ctx.dt); });

    _systems.Register(SystemPhase::FixedUpdate, "PhysicsSyncTransforms",
                      [this](SystemContext &ctx) { _physics.SyncTransforms(ctx.scene); })
        .After("PhysicsStep");

    // --- Default PostUpdate systems ---

    _systems.Register(SystemPhase::PostUpdate, "PropagateTransforms",
                      [](SystemContext &ctx) { Runtime::PropagateTransforms(ctx.scene); });

    _systems.Register(SystemPhase::PostUpdate, "CleanupDestroyTag",
                      [](SystemContext &ctx) { Runtime::DestroyMarked(ctx.scene); })
        .After("PropagateTransforms");

    OnGameStart();
}

void GameApplication::OnFixedUpdate(float dt)
{
    _systems.Run(SystemPhase::FixedUpdate, {_scene, dt, GetInput()});
}

void GameApplication::OnUpdate(float dt)
{
    _systems.Run(SystemPhase::PreUpdate,  {_scene, dt, GetInput()});
    _systems.Run(SystemPhase::Update,     {_scene, dt, GetInput()});
    _systems.Run(SystemPhase::PostUpdate, {_scene, dt, GetInput()});
}

void GameApplication::OnRender()
{
    OnGameRender();
}

void GameApplication::OnShutdown()
{
    OnGameShutdown();
}

} // namespace Assisi::App