/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// Tests for a world's contact events, as the systems in it read them, and for
/// installing a world's systems by name.
///
/// These run a real Jolt simulation rather than faking contacts, because the
/// things most likely to be wrong are exactly the things a fake would paper over:
/// which way the manifold normal points, whether the recorded velocity is the one
/// from *before* the solver absorbed the impact, and whether a body that stops
/// moving is reported as still touching or as having left.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <Assisi/App/World.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

using namespace Assisi::App;

namespace
{

constexpr float kStep = 1.f / 60.f;

/// A floor at y = 0 (top surface at y = 0.25) plus one dynamic box dropped above
/// it, both as real entities so contacts have entities to name. Returns the
/// falling entity.
Assisi::ECS::Entity BuildDropScene(World &world, glm::vec3 dropFrom)
{
    const auto spawn = [&world](glm::vec3 at, bool isStatic, glm::vec3 halfExtents)
                       {
                           const Assisi::ECS::Entity entity = world.scene.Create();
                           Assisi::ECS::Transform *transform = world.scene.Add<Assisi::ECS::Transform>(entity);
                           transform->position                 = at;

                           Assisi::Physics::Collider collider{};
                           collider.halfExtents = halfExtents;
                           (void)world.scene.Add<Assisi::Physics::Collider>(entity, collider);
                           if (!isStatic)
                           {
                               (void)world.scene.Add<Assisi::Physics::RigidBody>(entity);
                           }

                           world.physics.Reconcile();
                           return entity;
                       };

    (void)spawn({0.f, 0.f, 0.f}, /*isStatic=*/ true, {10.f, 0.25f, 10.f});
    return spawn(dropFrom, /*isStatic=*/ false, {0.5f, 0.5f, 0.5f});
}

/// Steps physics until something reports an Enter, then returns that step's
/// events. Returns an empty vector if nothing collided within @p maxSteps.
std::vector<Assisi::Physics::ContactEvent> StepUntilEnter(World &world, int32_t maxSteps = 240)
{
    for (int32_t i = 0; i < maxSteps; ++i)
    {
        world.physics.Update(kStep);

        const std::span<const Assisi::Physics::ContactEvent> events = world.physics.ContactEvents();
        const bool entered = std::any_of(events.begin(), events.end(),
                                         [](const Assisi::Physics::ContactEvent &e)
                                         { return e.phase == Assisi::Physics::ContactPhase::Enter; });
        if (entered)
            return {events.begin(), events.end()};
    }
    return {};
}

} // namespace

TEST_CASE("A landing body reports a contact from both sides, before the solver runs")
{
    WorldManager worlds;
    World &world = worlds.Create("Drop");

    const Assisi::ECS::Entity faller = BuildDropScene(world, {0.f, 3.f, 0.f});

    const std::vector<Assisi::Physics::ContactEvent> events = StepUntilEnter(world);
    REQUIRE_FALSE(events.empty());

    // Both participants are entities here, so the pair produces two records.
    CHECK(events.size() == 2u);

    const Assisi::Physics::ContactEvent *mine = nullptr;
    for (const Assisi::Physics::ContactEvent &event : events)
    {
        if (event.entity == faller)
            mine = &event;
    }
    REQUIRE(mine != nullptr);
    CHECK(mine->phase == Assisi::Physics::ContactPhase::Enter);
    CHECK_FALSE(mine->sensor);
    CHECK(mine->other != faller);
    CHECK(mine->other != Assisi::ECS::NullEntity);

    // The normal points away from what it hit, so a body landing on a floor sees
    // +Y regardless of which way round Jolt happened to order the pair. Getting
    // this backwards is the failure that would make a bounce drive bodies through
    // the ground, and it is invisible in a test that only checks "a contact
    // happened".
    CHECK(mine->normal.y > 0.9f);

    // Recorded before the solver ran: the body is still falling at the speed it
    // arrived with, not the ~0 it will have once the contact is resolved. From
    // 3 m up (a ~2.25 m drop to the floor's surface) that is roughly 6.6 m/s.
    CHECK(mine->velocity.y < -4.f);
}

TEST_CASE("A pair enters once and then stays, however long it rests")
{
    // With Stay asked for, Enter must fire exactly once and Stay must keep
    // coming — including after Jolt puts the body to sleep, at which point it
    // stops reporting the contact at all and the pair table has to carry it.
    WorldManager worlds;
    World &world = worlds.Create("Rests");
    world.physics.SetStayEventsReported(true);
    const Assisi::ECS::Entity ball = BuildDropScene(world, {0.f, 3.f, 0.f});

    REQUIRE_FALSE(StepUntilEnter(world).empty());

    int32_t enters = 0;
    int32_t stays  = 0;
    int32_t exits  = 0;
    for (int32_t i = 0; i < 600; ++i) // ten seconds of lying there
    {
        world.physics.Update(kStep);
        for (const Assisi::Physics::ContactEvent &event : world.physics.ContactEvents())
        {
            if (event.entity != ball)
                continue;
            if (event.phase == Assisi::Physics::ContactPhase::Enter)
                ++enters;
            else if (event.phase == Assisi::Physics::ContactPhase::Stay)
                ++stays;
            else
                ++exits;
        }
    }

    // It went to sleep somewhere in there — which is what makes the Stay count
    // interesting, since Jolt reports nothing for a sleeping pair.
    REQUIRE(world.physics.HasBody(ball));
    REQUIRE_FALSE(world.physics.IsBodyActive(ball));

    CHECK(stays == 600);
    CHECK(enters == 0);
    CHECK(exits == 0);
}

TEST_CASE("A pair that really separates reports one Exit")
{
    // The other half: dormancy must not swallow a genuine departure. Teleporting
    // the body away wakes it, so the pair is missing with a body awake — an Exit,
    // not a Stay.
    WorldManager worlds;
    World &world = worlds.Create("Leaves");
    const Assisi::ECS::Entity ball = BuildDropScene(world, {0.f, 3.f, 0.f});

    REQUIRE_FALSE(StepUntilEnter(world).empty());

    // Let it settle all the way to sleep first, so the Exit has to survive the
    // dormant case rather than being handed an already-awake pair.
    REQUIRE(world.physics.HasBody(ball));
    for (int32_t i = 0; i < 300; ++i)
    {
        world.physics.Update(kStep);
    }
    REQUIRE_FALSE(world.physics.IsBodyActive(ball));

    world.physics.Teleport(ball, Assisi::Physics::Pose{glm::quat{1.f, 0.f, 0.f, 0.f}, {0.f, 40.f, 0.f}});

    int32_t exits = 0;
    for (int32_t i = 0; i < 20; ++i)
    {
        world.physics.Update(kStep);
        for (const Assisi::Physics::ContactEvent &event : world.physics.ContactEvents())
        {
            if (event.entity == ball && event.phase == Assisi::Physics::ContactPhase::Exit)
                ++exits;
        }
    }

    CHECK(exits == 1);
}

TEST_CASE("ApplySystems refuses a name this build does not declare")
{
    // A level naming a system that is not here is a level that will run without
    // it — the silent failure the whole design opens with. So the load fails
    // rather than the world quietly running short.
    WorldManager worlds;
    World &world = worlds.Create("Typo");
    const std::vector<std::string> names{"NoSuchSystemAnywhere"};

    CHECK_FALSE(worlds.ApplySystems(world, names, "levels/Test.alvl"));

    // Recorded verbatim even so: a save must not rewrite the author's list with
    // whatever happened to install.
    CHECK(world.systemNames == names);
}

TEST_CASE("ApplySystems leaves the running systems alone when it refuses")
{
    // A refused call is a no-op on what is actually running: one bad name must
    // not leave the world running *nothing*, which is worse than the state it
    // was asked to replace.
    WorldManager worlds;
    World &world = worlds.Create("KeepsWhatItHas");

    const std::vector<std::string> good{"Counter"};
    REQUIRE(worlds.ApplySystems(world, good, "levels/Good.alvl"));
    REQUIRE(world.systems.Has("Counter"));

    const std::vector<std::string> bad{"Counter", "NoSuchSystemAnywhere"};
    CHECK_FALSE(worlds.ApplySystems(world, bad, "levels/Bad.alvl"));

    // Still running what it had. Note the bad list *contains* Counter — the point
    // is not that Counter survived by being re-installed, but that nothing was
    // torn down to begin with.
    CHECK(world.systems.Has("Counter"));
}
