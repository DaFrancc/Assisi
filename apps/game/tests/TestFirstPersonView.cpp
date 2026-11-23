/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestFirstPersonView.cpp
/// @brief The first-person template's camera rig: the view camera sits at the
///        character's eye height, blended between steps, and this machine's
///        player's camera is the one drawn.

#include "Player/CameraRig.hpp"

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/Matrix.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <doctest/doctest.h>

#include <cstdint>

using namespace Assisi;

namespace
{

constexpr float kStep = 1.f / 60.f;

App::SystemContext Context(App::World &world, Core::EventQueue &universal)
{
    return App::SystemContext{world, kStep, /*simTick=*/ 0, nullptr, nullptr, universal, true, nullptr};
}

/// A character on a floor, with a view camera authored somewhere else on
/// purpose: the height is the character's to set.
struct Standing
{
    Core::EventQueue universal{Core::EventScope::Universal};
    App::WorldManager worlds{Physics::NoCollisionAssets()};
    App::World &world = worlds.Create("View");
    ECS::Entity character = ECS::NullEntity;
    ECS::Entity eye = ECS::NullEntity;

    Standing()
    {
        ECS::Scene &scene = world.scene;
        const ECS::Entity floor = scene.Create();
        (void)scene.Add<ECS::Transform>(floor, ECS::Transform{.position = {0.f, -0.5f, 0.f}});
        Physics::Collider collider{};
        collider.halfExtents = {20.f, 0.5f, 20.f};
        (void)scene.Add<Physics::Collider>(floor, collider);

        character = scene.Create();
        eye = scene.Create();
        (void)scene.Add<ECS::Transform>(character);
        (void)scene.Add<Physics::Character>(character);
        (void)scene.Add<Physics::CharacterLocomotion>(character);
        (void)scene.Add<Physics::SourceMovement>(character);
        (void)scene.Add<Game::ViewRig>(character, Game::ViewRig{.camera = eye});
        (void)scene.Add<ECS::Transform>(eye, ECS::Transform{.position = {0.f, 5.f, 0.f}});
        (void)scene.Add<Runtime::Camera>(eye);
        (void)scene.Add<ECS::Parent>(eye, ECS::Parent{.parent = character});
        world.physics.Reconcile();
    }

    /// One fixed step as the hosts run it: the physics, then the template's
    /// eye height, which reads the CharacterState the step left.
    void Step(Physics::Stance stance = Physics::Stance::Standing)
    {
        world.scene.GetMut<Physics::CharacterIntent>(character)->stance = stance;
        const ECS::FixedStepScope step(world.scene);
        world.physics.Update(kStep);
        App::SystemContext ctx = Context(world, universal);
        Game::EyeHeightSystem(ctx);
    }

    float EyeY() const { return static_cast<float>(world.scene.Get<ECS::Transform>(eye)->position.y); }
};

} // namespace

TEST_CASE("The view camera is placed at its character's eye height, blended between steps")
{
    Standing standing;
    const Physics::Character descriptor{};
    for (std::int32_t i = 0; i < 60; ++i)
    {
        standing.Step();
    }
    CHECK(standing.EyeY() == doctest::Approx(descriptor.eyeHeight));
    std::uint64_t tick = ECS::PropagateTransforms(standing.world.scene, 0);

    // One step into a crouch, a frame drawn half way between the two steps
    // sees the eye half way between them: it moves every frame, not every step.
    standing.Step(Physics::Stance::Crouching);
    const float afterOneStep = standing.world.scene.Get<Physics::CharacterState>(standing.character)->eyeHeight;
    REQUIRE(afterOneStep < descriptor.eyeHeight);
    CHECK(standing.EyeY() == doctest::Approx(afterOneStep));

    ECS::SetBlendAlpha(standing.world.scene, 0.5f);
    tick = ECS::PropagateTransforms(standing.world.scene, tick);
    const float feet = static_cast<float>(standing.world.scene.Get<ECS::Transform>(standing.character)->position.y);
    const float drawnEye =
        static_cast<float>(ECS::WorldPositionOf(*standing.world.scene.Get<ECS::WorldMatrix>(standing.eye)).y);
    CHECK(drawnEye - feet == doctest::Approx(0.5f * (descriptor.eyeHeight + afterOneStep)));

    for (std::int32_t i = 0; i < 60; ++i)
    {
        standing.Step(Physics::Stance::Crouching);
    }
    CHECK(standing.EyeY() == doctest::Approx(descriptor.crouchEyeHeight));
}

TEST_CASE("Only this machine's player's view camera is drawn")
{
    Core::EventQueue universal{Core::EventScope::Universal};
    App::WorldManager worlds{Physics::NoCollisionAssets()};
    App::World &world = worlds.Create("View");
    ECS::Scene &scene = world.scene;

    const auto spawn = [&scene](bool local)
    {
        const ECS::Entity character = scene.Create();
        const ECS::Entity eye = scene.Create();
        (void)scene.Add<ECS::Transform>(character);
        (void)scene.Add<Game::ViewRig>(character, Game::ViewRig{.camera = eye});
        if (local)
        {
            (void)scene.Add<App::LocalPlayer>(character);
        }
        (void)scene.Add<ECS::Transform>(eye);
        // Every blueprint's camera is authored active; each machine looks
        // through its own.
        (void)scene.Add<Runtime::Camera>(eye, Runtime::Camera{.isActive = true});
        return eye;
    };
    const ECS::Entity mine = spawn(/*local=*/ true);
    const ECS::Entity theirs = spawn(/*local=*/ false);

    App::SystemContext ctx = Context(world, universal);
    Game::PlayerViewSystem(ctx);
    CHECK(scene.Get<Runtime::Camera>(mine)->isActive);
    CHECK_FALSE(scene.Get<Runtime::Camera>(theirs)->isActive);
}
