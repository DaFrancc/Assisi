/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestContactEvents.cpp
/// @brief Enter, Stay and Exit, and the cases Jolt's own callbacks get wrong.
///
/// Jolt reports a contact as *removed* when a body falls asleep, and forbids
/// reading either body at that moment because one may already be destroyed. So
/// the phases here are not Jolt's callbacks renamed: they come from comparing
/// this step's touching pairs against last step's, after the step, on one thread.
/// The cases that matter are the ones where that difference shows — a resting
/// body, a waking body, and a body destroyed mid-contact.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include "PhysicsTestScene.hpp"

using namespace Assisi;
using Assisi::PhysicsTests::kStep;

namespace
{

/// Long enough for a dropped body to land and for Jolt to put it to sleep.
constexpr int32_t kSleepSteps = 300;

ECS::Entity Spawn(ECS::Scene &scene, glm::vec3 at, glm::vec3 halfExtents, bool isStatic)
{
    return PhysicsTests::AddBody(scene, at, PhysicsTests::Box(halfExtents, isStatic));
}

/// Floor with its top at y = 0, plus a box dropped from @p dropFrom.
ECS::Entity BuildDrop(ECS::Scene &scene, float dropFrom)
{
    (void)Spawn(scene, {0.f, -1.f, 0.f}, {20.f, 1.f, 20.f}, /*isStatic=*/ true);
    return Spawn(scene, {0.f, dropFrom, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false);
}

int32_t CountPhase(const Physics::PhysicsWorld &world, ECS::Entity entity, Physics::ContactPhase phase)
{
    int32_t seen = 0;
    for (const Physics::ContactEvent &event : world.ContactEvents())
    {
        if (event.entity == entity && event.phase == phase)
            ++seen;
    }
    return seen;
}

/// Steps until @p entity reports an Enter, returning the step it happened on, or
/// -1 if it never did.
int32_t StepUntilEnter(Physics::PhysicsWorld &world, ECS::Entity entity, int32_t maxSteps = 240)
{
    for (int32_t i = 0; i < maxSteps; ++i)
    {
        world.Update(kStep);
        if (CountPhase(world, entity, Physics::ContactPhase::Enter) > 0)
            return i;
    }
    return -1;
}

} // namespace

TEST_CASE("A pair reports Enter exactly once, however many contact points it has")
{
    // A box landing flat on a floor touches at four corners, and Jolt runs several
    // collision substeps per Update. All of that has to collapse into one event,
    // or a consumer counting entries counts corners.
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    const ECS::Entity box = BuildDrop(scene, 3.f);

    REQUIRE(StepUntilEnter(world, box) >= 0);
    CHECK(CountPhase(world, box, Physics::ContactPhase::Enter) == 1);
}

TEST_CASE("Stay is not reported unless asked for")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    REQUIRE_FALSE(world.StayEventsReported());
    const ECS::Entity box = BuildDrop(scene, 3.f);

    REQUIRE(StepUntilEnter(world, box) >= 0);
    int32_t stays = 0;
    for (int32_t i = 0; i < kSleepSteps; ++i)
    {
        world.Update(kStep);
        stays += CountPhase(world, box, Physics::ContactPhase::Stay);
    }
    CHECK(stays == 0);
}

TEST_CASE("A resting pair keeps reporting Stay after the body falls asleep")
{
    // The case Jolt's callbacks cannot express. Once the box sleeps, Jolt stops
    // reporting the pair entirely and fires its "contact removed" callback — so
    // anything built on that callback would announce the box had left the floor it
    // is still sitting on.
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    world.SetStayEventsReported(true);
    const ECS::Entity box = BuildDrop(scene, 3.f);

    REQUIRE(StepUntilEnter(world, box) >= 0);

    int32_t stays = 0;
    int32_t exits = 0;
    for (int32_t i = 0; i < kSleepSteps; ++i)
    {
        world.Update(kStep);
        stays += CountPhase(world, box, Physics::ContactPhase::Stay);
        exits += CountPhase(world, box, Physics::ContactPhase::Exit);
    }

    REQUIRE_FALSE(world.IsBodyActive(box)); // it really did go to sleep

    CHECK(stays == kSleepSteps);
    CHECK(exits == 0);
}

TEST_CASE("A body woken while still touching does not report a second Enter")
{
    // The other half of dormancy. Jolt re-adds the contact when the body wakes,
    // which reads as brand new; the pair table remembers it never ended.
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    const ECS::Entity box = BuildDrop(scene, 3.f);

    REQUIRE(StepUntilEnter(world, box) >= 0);
    for (int32_t i = 0; i < kSleepSteps; ++i)
        world.Update(kStep);

    REQUIRE_FALSE(world.IsBodyActive(box));

    // A nudge along the floor: it wakes and keeps touching.
    scene.GetMut<Physics::BodyState>(box)->linearVelocity = {0.2f, 0.f, 0.f};
    world.Reconcile();
    REQUIRE(world.IsBodyActive(box));

    int32_t enters = 0;
    for (int32_t i = 0; i < 30; ++i)
    {
        world.Update(kStep);
        enters += CountPhase(world, box, Physics::ContactPhase::Enter);
    }
    CHECK(enters == 0);
}

TEST_CASE("Destroying an entity reports one Exit for what its body was touching")
{
    // The event has to be built when the body goes and delivered on the next
    // step. A consumer holding "who is inside me" would otherwise keep a destroyed
    // entity in it forever.
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

    const ECS::Entity floor = Spawn(scene, {0.f, -1.f, 0.f}, {20.f, 1.f, 20.f}, /*isStatic=*/ true);
    const ECS::Entity box   = Spawn(scene, {0.f, 3.f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false);

    REQUIRE(StepUntilEnter(world, box) >= 0);

    scene.Destroy(box);
    scene.FlushDestroyed();

    world.Update(kStep);

    // Reported from the floor's point of view, which is the side still around to
    // act on it.
    int32_t exits = 0;
    for (const Physics::ContactEvent &event : world.ContactEvents())
    {
        if (event.entity == floor && event.other == box && event.phase == Physics::ContactPhase::Exit)
            ++exits;
    }
    CHECK(exits == 1);

    // And nothing lingers: the pair is gone, not merely reported gone.
    world.Update(kStep);
    CHECK(CountPhase(world, floor, Physics::ContactPhase::Exit) == 0);
    CHECK(CountPhase(world, floor, Physics::ContactPhase::Stay) == 0);
}

TEST_CASE("Events describe one step and are dropped at the next")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    const ECS::Entity box = BuildDrop(scene, 3.f);

    REQUIRE(StepUntilEnter(world, box) >= 0);
    const std::size_t landed = world.ContactEvents().size();
    REQUIRE(landed > 0u);

    // The next step replaces them rather than appending: a consumer running once
    // per fixed step must never see the same event twice.
    world.Update(kStep);
    CHECK(CountPhase(world, box, Physics::ContactPhase::Enter) == 0);
}

TEST_CASE("Clearing the scene drops every pair without inventing departures")
{
    // A world being emptied is not a world where things left each other, and there
    // is nothing left that could act on the event anyway — the entities are about
    // to mean something else entirely.
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    const ECS::Entity box = BuildDrop(scene, 3.f);

    REQUIRE(StepUntilEnter(world, box) >= 0);

    scene.Clear();
    world.Update(kStep);
    CHECK(world.ContactEvents().empty());
}

namespace
{

/// Steps until every body in @p bodies is asleep, and a while after: Jolt
/// reports a sleeping body's contacts as removed on the step after it sleeps,
/// so a case meant to start from a quiet sleeping pair has to wait that out.
/// False if they never settle.
bool StepUntilAsleep(Physics::PhysicsWorld &world, std::initializer_list<ECS::Entity> bodies)
{
    constexpr int32_t kMaxSteps = 1200;
    constexpr int32_t kQuietSteps = 10;
    for (int32_t i = 0; i < kMaxSteps; ++i)
    {
        world.Update(kStep);
        bool allAsleep = true;
        for (const ECS::Entity body : bodies)
        {
            allAsleep = allAsleep && !world.IsBodyActive(body);
        }
        if (allAsleep)
        {
            for (int32_t quiet = 0; quiet < kQuietSteps; ++quiet)
            {
                world.Update(kStep);
            }
            return true;
        }
    }
    return false;
}

int32_t CountPair(const Physics::PhysicsWorld &world, ECS::Entity entity, ECS::Entity other,
                  Physics::ContactPhase phase)
{
    int32_t seen = 0;
    for (const Physics::ContactEvent &event : world.ContactEvents())
    {
        if (event.entity == entity && event.other == other && event.phase == phase)
        {
            ++seen;
        }
    }
    return seen;
}

} // namespace

TEST_CASE("A settled pile reports nothing")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    const ECS::Entity floor = Spawn(scene, {0.f, -1.f, 0.f}, {20.f, 1.f, 20.f}, /*isStatic=*/ true);
    const ECS::Entity bottom = Spawn(scene, {0.f, 0.5f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false);
    const ECS::Entity middle = Spawn(scene, {0.f, 1.5f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false);
    const ECS::Entity top = Spawn(scene, {0.f, 2.5f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false);

    REQUIRE(StepUntilAsleep(world, {bottom, middle, top}));

    for (int32_t i = 0; i < 60; ++i)
    {
        world.Update(kStep);
        CHECK(world.ContactEvents().empty());
    }

    // Still touching, as far as anyone asking is concerned.
    CHECK(world.IsTouching(floor, bottom));
    CHECK(world.IsTouching(bottom, middle));
    CHECK(world.IsTouching(middle, top));
    CHECK_FALSE(world.IsTouching(floor, top));
}

TEST_CASE("A sleeping body moved away reports its Exit on the next step")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    const ECS::Entity box = BuildDrop(scene, 3.f);
    REQUIRE(StepUntilAsleep(world, {box}));

    world.Teleport(box, Physics::Pose{glm::quat(1.f, 0.f, 0.f, 0.f), {0.f, 10.f, 0.f}});
    world.Update(kStep);

    CHECK(CountPhase(world, box, Physics::ContactPhase::Exit) == 1);
    std::vector<ECS::Entity> touching;
    world.Touching(box, touching);
    CHECK(touching.empty());
}

TEST_CASE("A body leaving what it rests on reports its Exit")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    const ECS::Entity box = BuildDrop(scene, 3.f);
    REQUIRE(StepUntilEnter(world, box) >= 0);

    scene.GetMut<Physics::BodyState>(box)->linearVelocity = {0.f, 8.f, 0.f};
    int32_t exits = 0;
    for (int32_t i = 0; i < 30; ++i)
    {
        world.Update(kStep);
        exits += CountPhase(world, box, Physics::ContactPhase::Exit);
    }
    CHECK(exits == 1);
}

TEST_CASE("A character walking off what it stands on reports its Exit")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    const ECS::Entity ledge = Spawn(scene, {0.f, -0.5f, 0.f}, {1.f, 0.5f, 1.f}, /*isStatic=*/ true);
    const ECS::Entity character = PhysicsTests::AddCharacter(scene, {0.f, 0.f, 0.f});
    for (int32_t i = 0; i < 30; ++i)
    {
        world.Update(kStep);
    }
    REQUIRE(world.IsTouching(character, ledge));

    int32_t exits = 0;
    for (int32_t i = 0; i < 120; ++i)
    {
        PhysicsTests::Drive(scene, character, {4.f, 0.f, 0.f}, /*jump=*/ false);
        world.Update(kStep);
        exits += CountPair(world, character, ledge, Physics::ContactPhase::Exit);
    }
    CHECK(exits == 1);
    CHECK_FALSE(world.IsTouching(character, ledge));
}

TEST_CASE("A body woken by a neighbour keeps touching what it rests on")
{
    // The bottom box wakes because the top one does, partway through a step, and
    // the step that wakes it never tested its contact with the floor. That pair
    // must not read as ended.
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    const ECS::Entity floor = Spawn(scene, {0.f, -1.f, 0.f}, {20.f, 1.f, 20.f}, /*isStatic=*/ true);
    const ECS::Entity bottom = Spawn(scene, {0.f, 0.5f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false);
    const ECS::Entity top = Spawn(scene, {0.f, 1.5f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false);
    REQUIRE(StepUntilAsleep(world, {bottom, top}));

    scene.GetMut<Physics::BodyState>(top)->linearVelocity = {0.3f, 0.f, 0.f};
    int32_t changes = 0;
    bool woke = false;
    for (int32_t i = 0; i < 30; ++i)
    {
        world.Update(kStep);
        woke = woke || world.IsBodyActive(bottom);
        changes += CountPair(world, bottom, floor, Physics::ContactPhase::Exit);
        changes += CountPair(world, bottom, floor, Physics::ContactPhase::Enter);
    }
    REQUIRE(woke);
    CHECK(changes == 0);
    CHECK(world.IsTouching(bottom, floor));
}

TEST_CASE("An Enter says where the bodies touch and which pieces did")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    const ECS::Entity box = BuildDrop(scene, 3.f);
    REQUIRE(StepUntilEnter(world, box) >= 0);

    bool found = false;
    for (const Physics::ContactEvent &event : world.ContactEvents())
    {
        if (event.entity != box || event.phase != Physics::ContactPhase::Enter)
        {
            continue;
        }
        found = true;

        // The floor's top face, under the box.
        CHECK(event.point.y == doctest::Approx(0.f).epsilon(0.05));
        CHECK(std::abs(event.point.x) <= 0.5f);
        CHECK(std::abs(event.point.z) <= 0.5f);
        CHECK(event.piece == box);
        CHECK(event.otherPiece == event.other);
    }
    CHECK(found);
}

TEST_CASE("Touching lists everything resting on a body")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    const ECS::Entity floor = Spawn(scene, {0.f, -1.f, 0.f}, {20.f, 1.f, 20.f}, /*isStatic=*/ true);
    const ECS::Entity left = Spawn(scene, {-3.f, 0.5f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false);
    const ECS::Entity right = Spawn(scene, {3.f, 0.5f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false);
    for (int32_t i = 0; i < 10; ++i)
    {
        world.Update(kStep);
    }

    std::vector<ECS::Entity> touching;
    world.Touching(floor, touching);
    CHECK(touching.size() == 2u);
    CHECK(std::find(touching.begin(), touching.end(), left) != touching.end());
    CHECK(std::find(touching.begin(), touching.end(), right) != touching.end());
    CHECK_FALSE(world.IsTouching(left, right));
}
