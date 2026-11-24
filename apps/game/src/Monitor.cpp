/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "Monitor.hpp"

#include <Assisi/App/SceneCamera.hpp>
#include <Assisi/App/ScreenSurfaces.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

#include <optional>
#include <vector>

namespace Game
{

namespace
{

/// How far from the camera a monitor can be used, in metres.
constexpr float kReach = 3.f;

/// The way a camera looks, in its own space.
constexpr glm::vec4 kForward{0.f, 0.f, -1.f, 0.f};

} // namespace

void UseMonitorSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.input == nullptr || ctx.ui == nullptr || ctx.ui->InUse() != nullptr)
    {
        return;
    }
    if (!ctx.input->IsKeyPressed(Assisi::Window::Key::E))
    {
        return;
    }
    const std::optional<Assisi::App::SceneView> view = Assisi::App::ActiveSceneCamera(ctx.world.scene);
    if (!view)
    {
        return;
    }
    const glm::dvec3 eye = Assisi::ECS::WorldPositionOf(view->world);
    const glm::vec3 forward = glm::vec3(Assisi::ECS::MatrixFrom(view->world, eye) * kForward);
    // Every body along the look, so the player's own capsule, which the camera
    // sits in, does not hide the monitor.
    std::vector<Assisi::Physics::QueryHit> hits;
    ctx.world.physics.CastRayAll(eye, glm::normalize(forward) * kReach, Assisi::Physics::CollisionFilter{},
                                 view->entity, hits);
    for (const Assisi::Physics::QueryHit &hit : hits)
    {
        if (ctx.world.scene.Has<Assisi::App::ScreenSurface>(hit.entity) &&
            Assisi::App::UseScreenSurface(ctx, hit.entity))
        {
            return;
        }
    }
}

} // namespace Game
