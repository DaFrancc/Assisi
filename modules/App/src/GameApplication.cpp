/// @file GameApplication.cpp

#include <Assisi/App/GameApplication.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/Lifecycle.hpp>

#include <nlohmann/json.hpp>

#include <fstream>

namespace Assisi::App
{

void GameApplication::OnStart()
{
    // --- Load action map from game.json "input.actions" section ---

    const auto pathResult = Core::AssetSystem::Resolve("game.json");
    if (pathResult)
    {
        std::ifstream file(pathResult.value());
        if (file.is_open())
        {
            try
            {
                const auto json = nlohmann::json::parse(file);
                if (json.contains("input") && json.at("input").contains("actions"))
                    _actions.LoadFromJson(json.at("input").at("actions"));
            }
            catch (const nlohmann::json::exception &e)
            {
                Core::Log::Warn("Failed to load input bindings from game.json: {}", e.what());
            }
        }
    }

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
    _systems.Run(SystemPhase::FixedUpdate, {_scene, dt, GetInput(), _actions});
}

void GameApplication::OnUpdate(float dt)
{
    _systems.Run(SystemPhase::PreUpdate,  {_scene, dt, GetInput(), _actions});
    _systems.Run(SystemPhase::Update,     {_scene, dt, GetInput(), _actions});
    _systems.Run(SystemPhase::PostUpdate, {_scene, dt, GetInput(), _actions});
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