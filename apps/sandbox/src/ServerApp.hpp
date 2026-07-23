/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ServerApp.hpp
/// @brief The `--server` mode of the sandbox: a headless, simulation-only
/// Application.
///
/// Deliberately *not* the editor running with its window switched off. A
/// dedicated server is a different program with a different job — load a level,
/// step physics at a fixed rate, and eventually replicate the result — and
/// pretending otherwise would mean making every editor panel, gizmo, and
/// selection overlay tolerate a null renderer for no benefit.
///
/// This is the Stage-2 proof of the headless split: it exercises
/// Application::InitializeCore() with the presentation half never brought up.
/// Networking is not wired in here yet; that is Stage 5-6.

#include <Assisi/App/Application.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include <cstdint>
#include <string>

namespace Sandbox
{

class ServerApp final : public Assisi::App::Application
{
  public:
    /// @param startupLevel Virtual path of a level to load at startup
    ///   (e.g. "levels/Materials.alvl"). Empty runs an empty world, which is
    ///   still a useful smoke test of the loop.
    /// @param tickLimit Stop after this many fixed ticks; 0 runs until
    ///   interrupted. Bounded runs are what make this testable from a script.
    explicit ServerApp(std::string startupLevel, std::uint64_t tickLimit);

  protected:
    void OnStart() override;
    void OnFixedUpdate(float dt) override;
    void OnUpdate(float dt) override;
    void OnShutdown() override;
    void FlushDeferred() override;

  private:
    std::string   _startupLevel;
    std::uint64_t _tickLimit = 0;

    Assisi::ECS::Scene              _scene;
    Assisi::Physics::PhysicsWorld   _physics;

    /// Wall-clock of the last status log and the tick it was taken at, so the
    /// log can report the *measured* tick rate rather than the configured one —
    /// the whole point of the status line is to catch a loop that is not
    /// keeping up.
    double        _lastReportSeconds = 0.0;
    std::uint64_t _lastReportTick    = 0;
};

} // namespace Sandbox
