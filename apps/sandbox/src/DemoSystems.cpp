/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "DemoSystems.hpp"

#include <Assisi/App/BlueprintVerbs.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

namespace Sandbox
{

void SpinDemoSystem(Assisi::App::SystemContext &ctx)
{
    constexpr float kRadiansPerSecond = 1.0f;
    const glm::quat step = glm::angleAxis(kRadiansPerSecond * ctx.dt, glm::vec3(0.f, 1.f, 0.f));

    Assisi::ECS::Scene &scene = ctx.world.scene;
    for (auto [entity, transform] :
         scene.Query<Assisi::ECS::Transform>(Assisi::ECS::Without<Assisi::Physics::RigidBodyDescriptor>{}))
    {
        (void)transform;
        // Transform is ACOMP(tracked), and the query hands out an unstamped
        // reference: write through GetMut so PropagateTransforms actually sees
        // the change, or the world matrix keeps the old pose.
        if (Assisi::ECS::Transform *mutable_ = scene.GetMut<Assisi::ECS::Transform>(entity))
            mutable_->rotation = step * mutable_->rotation;
    }
}

void InputDemoSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.input == nullptr) // headless host: no devices to read
        return;
    if (ctx.input->IsKeyPressed(Assisi::Window::Key::Space))
    {
        Assisi::Core::Log::Info("InputDemo: space in world '{}' (active={}).", ctx.world.name,
                                ctx.isActiveWorld);
    }

}

void BouncerSpawnSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.input == nullptr) // headless host: no devices to read
        return;

    // Bouncer's only member is replicated, so on a host this is the shortest
    // path to watching a spawned instance reach a client — record, block,
    // expansion and all. See the header for what it does *not* prove.
    if (ctx.input->IsKeyPressed(Assisi::Window::Key::F3))
    {
        Assisi::ECS::Transform at;
        // Above the pile, and nudged off-centre per press so a held key makes a
        // heap rather than a tower of coincident bodies fighting each other.
        const float spread = static_cast<float>(ctx.simTick % 7u) * 0.25f - 0.75f;
        at.position        = {spread, 8.f, spread * 0.5f};

        const std::optional<Assisi::ECS::InstanceId> spawned =
            Assisi::App::SpawnBlueprint(ctx.world, "blueprints/Bouncer.abp", at);
        if (spawned.has_value())
        {
            Assisi::Core::Log::Info("InputDemo: spawned Bouncer as instance {} in world '{}'.", *spawned,
                                    ctx.world.name);
        }
        else
        {
            Assisi::Core::Log::Warn("InputDemo: could not spawn blueprints/Bouncer.abp.");
        }
    }
}

} // namespace Sandbox
