#pragma once

/// @file App/GameApplication.hpp
/// @brief Base class for games and the editor.
///
/// GameApplication sits between the thin Application base (window, loop, ImGui)
/// and your game code.  It owns the scene, physics world, and system registry,
/// and pre-wires a set of default systems so a new project works out of the box.
///
/// Pre-wired default systems:
///   FixedUpdate: PhysicsStep → PhysicsSyncTransforms
///   PostUpdate:  PropagateTransforms → CleanupDestroyTag
///
/// Override OnGameStart() to register your own systems and do one-time setup.
/// Override OnGameRender() to submit draw calls (receives no automatic render
/// context — build a RenderContext from your camera entity and call
/// _systems.Run(SystemPhase::Render, ctx) yourself).
///
/// @par Minimal example
/// @code
/// class MyGame : public GameApplication
/// {
///     void OnGameStart() override
///     {
///         _systems.Register(SystemPhase::Update, "Movement", &MovementSystem);
///         _systems.Register(SystemPhase::Render, "Draw",
///             [this](RenderContext& ctx) {
///                 Runtime::DrawScene(ctx.scene, ctx.view, ctx.projection, _shader);
///             });
///     }
///
///     void OnGameRender() override
///     {
///         const glm::mat4 view = Runtime::ViewMatrix(...);
///         _systems.Run(SystemPhase::Render, { _scene, 0.f, view, _projection });
///     }
/// };
/// @endcode

#include <Assisi/App/Application.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>
#include <Assisi/Window/ActionMap.hpp>

namespace Assisi::App
{

class GameApplication : public Application
{
  public:
    GameApplication()           = default;
    ~GameApplication() override = default;

    GameApplication(const GameApplication &)            = delete;
    GameApplication &operator=(const GameApplication &) = delete;

  protected:
    /// @brief Called after default systems are registered.  Register your own
    ///        systems and perform one-time game setup here.
    virtual void OnGameStart() {}

    /// @brief Called after all system phases (PreUpdate/Update/PostUpdate) have run.
    ///        Override to submit render commands or call
    ///        _systems.Run(SystemPhase::Render, ctx).
    virtual void OnGameRender() {}

    /// @brief Called after the main loop exits, before Application teardown.
    virtual void OnGameShutdown() {}

    ECS::Scene              _scene;
    Physics::PhysicsWorld   _physics;
    SystemRegistry          _systems;
    Window::ActionMap       _actions;

  private:
    void OnStart()           final;
    void OnFixedUpdate(float dt) final;
    void OnUpdate(float dt)      final;
    void OnRender()              final;
    void OnShutdown()            final;
};

} // namespace Assisi::App