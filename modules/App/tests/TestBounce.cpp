/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// Tests for PhysicsWorld's contact events and for App::BounceSystem, the first
/// consumer of them.
///
/// These run a real Jolt simulation rather than faking contacts, because the
/// things most likely to be wrong are exactly the things a fake would paper over:
/// which way the manifold normal points, whether the recorded velocity is the one
/// from *before* the solver absorbed the impact, and whether a body that stops
/// moving is reported as still touching or as having left.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <Assisi/App/PhysicsSystems.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Window/ActionMap.hpp>

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

                           Assisi::Physics::RigidBodyDescriptor descriptor{};
                           descriptor.halfExtents = halfExtents;
                           descriptor.isStatic    = isStatic;
                           (void)world.scene.Add<Assisi::Physics::RigidBodyDescriptor>(entity, descriptor);

                           (void)world.physics.AddBodyFromDescriptor(world.scene, entity, *transform, descriptor);
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
        world.physics.CaptureState();

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
    // The distinction the whole event model exists for. The old contact log
    // reported the landing step and then went silent, so nothing could ask
    // "is it still there?". Enter must fire exactly once and Stay must keep
    // coming — including after Jolt puts the body to sleep, at which point it
    // stops reporting the contact at all and the pair table has to carry it.
    WorldManager worlds;
    World &world = worlds.Create("Rests");
    const Assisi::ECS::Entity ball = BuildDropScene(world, {0.f, 3.f, 0.f});

    REQUIRE_FALSE(StepUntilEnter(world).empty());

    int32_t enters = 0;
    int32_t stays  = 0;
    int32_t exits  = 0;
    for (int32_t i = 0; i < 600; ++i) // ten seconds of lying there
    {
        world.physics.Update(kStep);
        world.physics.CaptureState();
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
    const Assisi::Physics::RigidBody *body = world.scene.Get<Assisi::Physics::RigidBody>(ball);
    REQUIRE(body != nullptr);
    REQUIRE_FALSE(world.physics.IsBodyActive(*body));

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
    const Assisi::Physics::RigidBody *body = world.scene.Get<Assisi::Physics::RigidBody>(ball);
    REQUIRE(body != nullptr);
    for (int32_t i = 0; i < 300; ++i)
    {
        world.physics.Update(kStep);
        world.physics.CaptureState();
    }
    REQUIRE_FALSE(world.physics.IsBodyActive(*body));

    world.physics.SetBodyTransform(*body, {0.f, 40.f, 0.f}, glm::quat{1.f, 0.f, 0.f, 0.f});

    int32_t exits = 0;
    for (int32_t i = 0; i < 20; ++i)
    {
        world.physics.Update(kStep);
        world.physics.CaptureState();
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

namespace
{

/// The impact speed of the ball's first Enter, and the vertical velocity the
/// bounce system left it with on that same step. Both zero if it never landed.
struct BounceOutcome
{
    float impactSpeed = 0.f;
    float launchSpeed = 0.f;
};

/// Runs the loop OnFixedUpdate uses — systems first, then the step — until the
/// ball's first Enter has been seen and acted on.
BounceOutcome RunUntilBounce(WorldManager &worlds, World &world, Assisi::ECS::Entity ball,
                             int32_t maxSteps = 240)
{
    Assisi::Core::EventQueue events;
    Assisi::Window::ActionMap actions;

    BounceOutcome outcome;
    for (int32_t i = 0; i < maxSteps && outcome.launchSpeed == 0.f; ++i)
    {
        SystemContext ctx{world, kStep, /*simTick=*/ 0, nullptr, &actions, events, true, &worlds};

        for (const Assisi::Physics::ContactEvent &event : world.physics.ContactEvents())
        {
            if (event.entity == ball && event.phase == Assisi::Physics::ContactPhase::Enter &&
                outcome.impactSpeed == 0.f)
            {
                outcome.impactSpeed = -event.velocity.y;
            }
        }

        BounceSystem(ctx);

        if (outcome.impactSpeed > 0.f)
        {
            const Assisi::Physics::RigidBody *body = world.scene.Get<Assisi::Physics::RigidBody>(ball);
            REQUIRE(body != nullptr);
            outcome.launchSpeed = world.physics.GetBodyVelocity(*body).first.y;
        }

        world.physics.Update(kStep);
        world.physics.CaptureState();
    }
    return outcome;
}

} // namespace

TEST_CASE("BounceSystem sends a landing body back up, scaled by rebound")
{
    // The end-to-end behaviour: drop a box on a floor, run the system the way a
    // FixedUpdate registration would (before the step), and check it leaves going
    // up rather than staying put.
    WorldManager worlds;
    World &world = worlds.Create("Bouncy");

    const Assisi::ECS::Entity ball = BuildDropScene(world, {0.f, 3.f, 0.f});
    (void)world.scene.Add<Assisi::Physics::Bounce>(ball, Assisi::Physics::Bounce{.rebound = 0.5f});

    const BounceOutcome outcome = RunUntilBounce(worlds, world, ball);

    REQUIRE(outcome.impactSpeed > 4.f); // it really did arrive at speed
    CHECK(outcome.launchSpeed > 0.f);   // ...and left going the other way
    CHECK(outcome.launchSpeed == doctest::Approx(outcome.impactSpeed * 0.5f).epsilon(0.01)); // at half
}

TEST_CASE("rebound of zero stops a body dead, and a negative one is clamped to that")
{
    // 0 and "negative" are the two ends the component's contract names. A level
    // file is just text, so the negative case has to be handled in the system, not
    // only by the inspector's floor.
    for (const float rebound : {0.f, -2.f})
    {
        WorldManager worlds;
        World &world = worlds.Create("Dead");

        const Assisi::ECS::Entity ball = BuildDropScene(world, {0.f, 3.f, 0.f});
        (void)world.scene.Add<Assisi::Physics::Bounce>(ball, Assisi::Physics::Bounce{.rebound = rebound});

        Assisi::Core::EventQueue events;
        Assisi::Window::ActionMap actions;

        bool  bounced      = false;
        float afterContact = 1.f;

        for (int32_t i = 0; i < 240 && !bounced; ++i)
        {
            SystemContext ctx{world, kStep, /*simTick=*/ 0, nullptr, &actions, events, true, &worlds};

            const std::span<const Assisi::Physics::ContactEvent> stepEvents = world.physics.ContactEvents();
            const bool entered = std::any_of(stepEvents.begin(), stepEvents.end(),
                                             [ball](const Assisi::Physics::ContactEvent &e)
                                             {
                                                 return e.entity == ball &&
                                                        e.phase == Assisi::Physics::ContactPhase::Enter;
                                             });
            BounceSystem(ctx);

            if (entered)
            {
                const Assisi::Physics::RigidBody *body = world.scene.Get<Assisi::Physics::RigidBody>(ball);
                REQUIRE(body != nullptr);
                afterContact = world.physics.GetBodyVelocity(*body).first.y;
                bounced      = true;
            }

            world.physics.Update(kStep);
            world.physics.CaptureState();
        }

        REQUIRE(bounced);
        // Not merely "not upwards" — exactly zero, i.e. the system wrote the
        // clamped result rather than leaving the incoming velocity alone.
        CHECK(afterContact == doctest::Approx(0.f));
    }
}

TEST_CASE("A body already at rest never launches itself, even at rebound > 1")
{
    // The runaway: a settled body still exchanges the occasional new contact with
    // the surface it is lying on, at a closing speed of near enough nothing. With
    // rebound above 1 that noise gets amplified, the body leaves the ground a
    // little, lands with slightly more speed, and grows itself a bounce out of
    // nowhere. Starting from rest is the whole point of the case — there is no
    // impact here to respond to.
    WorldManager worlds;
    World &world = worlds.Create("Jittery");

    // Floor top is at y = 0.25 and the box's half-extent is 0.5, so this is its
    // resting height: it is already touching, with zero velocity.
    const Assisi::ECS::Entity ball = BuildDropScene(world, {0.f, 0.75f, 0.f});
    (void)world.scene.Add<Assisi::Physics::Bounce>(ball, Assisi::Physics::Bounce{.rebound = 2.f});

    Assisi::Core::EventQueue events;
    Assisi::Window::ActionMap actions;

    float highest = 0.f;
    for (int32_t i = 0; i < 600; ++i) // ten seconds of lying still
    {
        SystemContext ctx{world, kStep, /*simTick=*/ 0, nullptr, &actions, events, true, &worlds};
        BounceSystem(ctx);
        world.physics.Update(kStep);
        world.physics.CaptureState();

        const Assisi::Physics::RigidBody *body = world.scene.Get<Assisi::Physics::RigidBody>(ball);
        REQUIRE(body != nullptr);
        highest = std::max(highest, world.physics.GetBodyTransform(*body).first.y);
    }

    CHECK(highest < 0.8f); // never left the floor
}

TEST_CASE("What a settling nudge does at rebound > 1 depends on kMinBounceSpeed")
{
    // The reported bug, reproduced, and pinned to the constant that governs it.
    // Dropped from 9 cm above its resting height, the box's first contact closes
    // at ~0.98 m/s — a settling nudge, not an impact anyone would notice. What
    // happens next is entirely a function of kMinBounceSpeed, so this test asserts
    // both outcomes rather than picking one:
    //
    //   - Threshold above the nudge (1.0 m/s): ignored, and the box stays put.
    //   - Threshold below it (the current 1 mm/s): the nudge comes back at 1.47,
    //     then 2.04, 2.99, ... and the box climbs to ~18.8 m under its own power.
    //
    // Either way this fails loudly if the *mechanism* changes, and it means the
    // consequence of retuning the constant is written down in a place that runs.
    WorldManager worlds;
    World &world = worlds.Create("Runaway");

    const Assisi::ECS::Entity ball = BuildDropScene(world, {0.f, 0.84f, 0.f});
    (void)world.scene.Add<Assisi::Physics::Bounce>(ball, Assisi::Physics::Bounce{.rebound = 1.5f});

    Assisi::Core::EventQueue events;
    Assisi::Window::ActionMap actions;

    float firstClosingSpeed = 0.f;
    float highest           = 0.f;

    for (int32_t i = 0; i < 600; ++i) // ten seconds — long enough for a runaway to be obvious
    {
        SystemContext ctx{world, kStep, /*simTick=*/ 0, nullptr, &actions, events, true, &worlds};

        for (const Assisi::Physics::ContactEvent &event : world.physics.ContactEvents())
        {
            if (event.entity == ball && event.phase == Assisi::Physics::ContactPhase::Enter &&
                firstClosingSpeed == 0.f)
            {
                firstClosingSpeed = -glm::dot(event.velocity, event.normal);
            }
        }

        BounceSystem(ctx);
        world.physics.Update(kStep);
        world.physics.CaptureState();

        const Assisi::Physics::RigidBody *body = world.scene.Get<Assisi::Physics::RigidBody>(ball);
        REQUIRE(body != nullptr);
        highest = std::max(highest, world.physics.GetBodyTransform(*body).first.y);
    }

    // The seed really is a settling nudge, not a miss and not a real impact.
    REQUIRE(firstClosingSpeed > 0.5f);
    REQUIRE(firstClosingSpeed < 1.5f);

    if (kMinBounceSpeed > firstClosingSpeed)
    {
        // Rejected: resting height is 0.75 and it started 0.09 above that.
        CHECK(highest < 0.85f);
    }
    else
    {
        // Accepted, and it runs away — the documented cost of an epsilon set below
        // settling speeds. If this ever stops holding, the feedback path itself has
        // changed and the constant's guidance needs revisiting.
        CHECK(highest > 5.f);
    }
}

TEST_CASE("A real impact still bounces at rebound > 1, and gains speed")
{
    // The threshold must not have simply disabled the feature: dropped from a
    // proper height the box arrives well over the minimum and leaves faster than
    // it came in, which is what rebound above 1 is for.
    WorldManager worlds;
    World &world = worlds.Create("Hot");

    const Assisi::ECS::Entity ball = BuildDropScene(world, {0.f, 3.f, 0.f});
    (void)world.scene.Add<Assisi::Physics::Bounce>(ball, Assisi::Physics::Bounce{.rebound = 1.5f});

    const BounceOutcome outcome = RunUntilBounce(worlds, world, ball);

    REQUIRE(outcome.impactSpeed > 4.f);
    CHECK(outcome.launchSpeed > outcome.impactSpeed); // it left faster than it arrived
    CHECK(outcome.launchSpeed == doctest::Approx(outcome.impactSpeed * 1.5f).epsilon(0.01));
}

TEST_CASE("A body with no Bounce component is left alone")
{
    // The system's gate is the component, not the contact: everything else in a
    // world collides too, and none of it should be relaunched.
    WorldManager worlds;
    World &world = worlds.Create("Inert");

    const Assisi::ECS::Entity ball = BuildDropScene(world, {0.f, 3.f, 0.f});

    Assisi::Core::EventQueue events;
    Assisi::Window::ActionMap actions;

    float highestAfterLanding = -100.f;
    bool  landed              = false;

    for (int32_t i = 0; i < 240; ++i)
    {
        SystemContext ctx{world, kStep, /*simTick=*/ 0, nullptr, &actions, events, true, &worlds};
        if (!world.physics.ContactEvents().empty())
            landed = true;
        BounceSystem(ctx);

        world.physics.Update(kStep);
        world.physics.CaptureState();

        if (landed)
        {
            const Assisi::Physics::RigidBody *body = world.scene.Get<Assisi::Physics::RigidBody>(ball);
            REQUIRE(body != nullptr);
            highestAfterLanding =
                std::max(highestAfterLanding, world.physics.GetBodyVelocity(*body).first.y);
        }
    }

    REQUIRE(landed);
    CHECK(highestAfterLanding < 0.5f); // it settled; nothing threw it back up
}
