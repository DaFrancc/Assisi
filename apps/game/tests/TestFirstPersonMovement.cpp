/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestFirstPersonMovement.cpp
/// @brief The first-person template's movement: a player's controls become
///        what its character is asked to do.
///
/// The controls are written directly, as PlayerGather or a host relaying a
/// remote player would fill them in. What is checked is the translation into
/// the character's own frame, Walk shortening a full move, a jump asked for
/// once per press, and a character without controls left alone.

#include "Player/Movement.hpp"
#include "Player/PlayerActions.hpp"

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/PlayerSystems.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

#include <doctest/doctest.h>

using namespace Assisi;

namespace
{

constexpr float kStep = 1.f / 60.f;

/// Close enough for a direction that went through a quaternion.
constexpr double kTolerance = 1e-4;

/// A character the template moves, with nothing stepping its physics.
struct Mover
{
    Core::EventQueue universal{Core::EventScope::Universal};
    App::WorldManager worlds{Physics::NoCollisionAssets()};
    App::World &world = worlds.Create("Move");
    ECS::Entity character = ECS::NullEntity;

    explicit Mover(bool withControls = true)
    {
        ECS::Scene &scene = world.scene;
        character = scene.Create();
        (void)scene.Add<ECS::Transform>(character);
        (void)scene.Add<Physics::Character>(character);
        (void)scene.Add<Physics::CharacterIntent>(character);
        (void)scene.Add<App::Player>(character);
        (void)scene.Add<Game::Movement>(character);
        if (withControls)
        {
            (void)scene.Add<App::PlayerInput>(character);
        }
    }

    App::SystemContext Context()
    {
        return App::SystemContext{world, kStep, /*simTick=*/ 0, nullptr, nullptr, universal, true, nullptr};
    }

    App::PlayerInput &Controls() { return *world.scene.GetMut<App::PlayerInput>(character); }
    const Physics::CharacterIntent &Intent() const { return *world.scene.Get<Physics::CharacterIntent>(character); }

    /// One step's movement, then the engine clearing what the step used.
    void Step()
    {
        App::SystemContext ctx = Context();
        Game::MovementSystem(ctx);
        App::ClearUsedControls(world.scene);
    }
};

} // namespace

TEST_CASE("A player's move is taken in the character's own frame, along the ground")
{
    Mover mover;
    // Turned a quarter left: forward is -X.
    mover.world.scene.GetMut<ECS::Transform>(mover.character)->rotation =
        glm::angleAxis(glm::radians(90.f), glm::vec3(0.f, 1.f, 0.f));
    mover.Controls().move = {0.f, 1.f};

    mover.Step();
    CHECK(mover.Intent().move.x == doctest::Approx(-1.f).epsilon(kTolerance));
    CHECK(mover.Intent().move.y == doctest::Approx(0.f).epsilon(kTolerance));
    CHECK(mover.Intent().move.z == doctest::Approx(0.f).epsilon(kTolerance));
}

TEST_CASE("Holding Walk shortens a full move, and a stick pushed part way asks for less on its own")
{
    Mover mover;
    mover.Controls().move = {0.f, 1.f};
    mover.Controls().held = App::PlayerButtons::Of(Game::Actions::Walk);
    mover.Step();
    const float walkScale = mover.world.scene.Get<Game::Movement>(mover.character)->walkMoveScale;
    CHECK(glm::length(mover.Intent().move) == doctest::Approx(walkScale).epsilon(kTolerance));

    mover.Controls().held = App::PlayerButtons{};
    mover.Controls().move = {0.f, 0.5f};
    mover.Step();
    CHECK(glm::length(mover.Intent().move) == doctest::Approx(0.5f).epsilon(kTolerance));
}

TEST_CASE("A character with no controls is left to whatever else moves it")
{
    Mover npc(/*withControls=*/ false);
    npc.world.scene.GetMut<Physics::CharacterIntent>(npc.character)->move = {0.f, 0.f, 1.f};

    npc.Step();
    // An AI's intent survives: only a character with controls is moved.
    CHECK(npc.Intent().move.z == doctest::Approx(1.f));
}

TEST_CASE("A jump press asks for one jump, however long it is held")
{
    Mover mover;
    mover.Controls().held = App::PlayerButtons::Of(App::PlayerAction::Jump);
    mover.Controls().pressed = App::PlayerButtons::Of(App::PlayerAction::Jump);

    mover.Step();
    CHECK(mover.Intent().jump);

    // The physics step takes the request. Still held, nothing new was pressed.
    mover.world.scene.GetMut<Physics::CharacterIntent>(mover.character)->jump = false;
    mover.Step();
    CHECK_FALSE(mover.Intent().jump);
}

TEST_CASE("Holding crouch asks the character to crouch, and letting go to stand")
{
    Mover mover;
    mover.Controls().held = App::PlayerButtons::Of(App::PlayerAction::Crouch);
    mover.Step();
    CHECK(mover.Intent().stance == Physics::Stance::Crouching);

    mover.Controls().held = App::PlayerButtons{};
    mover.Step();
    CHECK(mover.Intent().stance == Physics::Stance::Standing);
}

TEST_CASE("Holding sprint asks the character to sprint, for as long as it is held")
{
    Mover mover;
    mover.Controls().held = App::PlayerButtons::Of(App::PlayerAction::Sprint);
    mover.Step();
    CHECK(mover.Intent().sprint);

    mover.Controls().held = App::PlayerButtons{};
    mover.Step();
    CHECK_FALSE(mover.Intent().sprint);
}
