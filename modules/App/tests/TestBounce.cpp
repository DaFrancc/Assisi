/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// Tests for the contact log (Physics::PhysicsWorld's opt-in record of collisions
/// that began during a step) and for App::BounceSystem, the first consumer of it.
///
/// These run a real Jolt simulation rather than faking contacts, because the
/// things most likely to be wrong are exactly the things a fake would paper over:
/// which way the manifold normal points, and whether the recorded velocity is the
/// one from *before* the solver absorbed the impact.

#include <doctest/doctest.h>

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
        Assisi::ECS::Transform   *transform = world.scene.Add<Assisi::ECS::Transform>(entity);
        transform->position                 = at;

        Assisi::Physics::RigidBodyDescriptor descriptor{};
        descriptor.halfExtents = halfExtents;
        descriptor.isStatic    = isStatic;
        (void)world.scene.Add<Assisi::Physics::RigidBodyDescriptor>(entity, descriptor);

        (void)world.physics.AddBodyFromDescriptor(world.scene, entity, *transform, descriptor);
        return entity;
    };

    (void)spawn({0.f, 0.f, 0.f}, /*isStatic=*/true, {10.f, 0.25f, 10.f});
    return spawn(dropFrom, /*isStatic=*/false, {0.5f, 0.5f, 0.5f});
}

/// Steps physics until the contact log is non-empty, then returns the log for
/// that step. Returns an empty vector if nothing collided within @p maxSteps.
std::vector<Assisi::Physics::Contact> StepUntilContact(World &world, int32_t maxSteps = 240)
{
    for (int32_t i = 0; i < maxSteps; ++i)
    {
        world.physics.Update(kStep);
        world.physics.CaptureState();
        const std::span<const Assisi::Physics::Contact> contacts = world.physics.Contacts();
        if (!contacts.empty())
            return {contacts.begin(), contacts.end()};
    }
    return {};
}

} // namespace

TEST_CASE("Contact reporting is off by default and records nothing")
{
    // The whole point of the opt-in: a world nobody asked for contacts in must
    // not pay for a log, and must not quietly accumulate one either.
    WorldManager worlds;
    World       &world = worlds.Create("Quiet");
    CHECK_FALSE(world.physics.IsContactReporting());

    (void)BuildDropScene(world, {0.f, 3.f, 0.f});

    for (int32_t i = 0; i < 240; ++i)
    {
        world.physics.Update(kStep);
        world.physics.CaptureState();
        REQUIRE(world.physics.Contacts().empty());
    }
}

TEST_CASE("A landing body reports a contact from both sides, before the solver runs")
{
    WorldManager worlds;
    World       &world = worlds.Create("Drop");
    world.physics.SetContactReporting(true);
    CHECK(world.physics.IsContactReporting());

    const Assisi::ECS::Entity faller = BuildDropScene(world, {0.f, 3.f, 0.f});

    const std::vector<Assisi::Physics::Contact> contacts = StepUntilContact(world);
    REQUIRE_FALSE(contacts.empty());

    // Both participants are entities here, so the pair produces two records.
    CHECK(contacts.size() == 2u);

    const Assisi::Physics::Contact *mine = nullptr;
    for (const Assisi::Physics::Contact &contact : contacts)
    {
        if (contact.entity == faller)
            mine = &contact;
    }
    REQUIRE(mine != nullptr);
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

TEST_CASE("The contact log describes exactly one step")
{
    // A consumer runs once per fixed step and must never see the same impact
    // twice — which is what clearing at the top of Update() buys, and what a log
    // that merely appended would get wrong.
    WorldManager worlds;
    World       &world = worlds.Create("Once");
    world.physics.SetContactReporting(true);
    (void)BuildDropScene(world, {0.f, 3.f, 0.f});

    REQUIRE_FALSE(StepUntilContact(world).empty());

    // The very next step reports nothing: the body is now resting, and a resting
    // contact persists rather than being added again.
    world.physics.Update(kStep);
    CHECK(world.physics.Contacts().empty());
}

TEST_CASE("Turning contact reporting off stops recording and drops the log")
{
    WorldManager worlds;
    World       &world = worlds.Create("Hush");
    world.physics.SetContactReporting(true);
    (void)BuildDropScene(world, {0.f, 3.f, 0.f});

    REQUIRE_FALSE(StepUntilContact(world).empty());
    CHECK_FALSE(world.physics.Contacts().empty()); // still there until something clears it

    world.physics.SetContactReporting(false);
    CHECK_FALSE(world.physics.IsContactReporting());
    CHECK(world.physics.Contacts().empty());
}

TEST_CASE("ApplySystems resets contact reporting so it cannot leak between levels")
{
    // A system that wants contact reporting turns it on for itself, so
    // re-targeting a world at a list that does not want it must switch it back
    // off. Otherwise the first bouncy level opened in a session taxes every level
    // after it with a contact log nothing reads.
    WorldManager worlds;
    World       &world = worlds.Create("Reused");

    world.physics.SetContactReporting(true);
    CHECK(world.physics.IsContactReporting());

    // Any re-target clears it, including to the empty list.
    CHECK(worlds.ApplySystems(world, {}, "(test)"));
    CHECK_FALSE(world.physics.IsContactReporting());
}

TEST_CASE("ApplySystems refuses a name this build does not declare")
{
    // A level naming a system that is not here is a level that will run without
    // it — the silent failure the whole design opens with. So the load fails
    // rather than the world quietly running short.
    WorldManager             worlds;
    World                   &world = worlds.Create("Typo");
    const std::vector<std::string> names{"NoSuchSystemAnywhere"};

    CHECK_FALSE(worlds.ApplySystems(world, names, "levels/Test.alvl"));

    // Recorded verbatim even so: a save must not rewrite the author's list with
    // whatever happened to install.
    CHECK(world.systemNames == names);
}

TEST_CASE("BounceSystem sends a landing body back up, scaled by rebound")
{
    // The end-to-end behaviour: drop a box on a floor, run the system the way a
    // FixedUpdate registration would (before the step), and check it leaves going
    // up rather than staying put.
    WorldManager worlds;
    World       &world = worlds.Create("Bouncy");
    world.physics.SetContactReporting(true);

    const Assisi::ECS::Entity ball = BuildDropScene(world, {0.f, 3.f, 0.f});
    (void)world.scene.Add<Assisi::Physics::Bounce>(ball, Assisi::Physics::Bounce{.rebound = 0.5f});

    Assisi::Core::EventQueue events;
    Assisi::Window::ActionMap actions;

    float impactSpeed = 0.f;
    float launchSpeed = 0.f;

    for (int32_t i = 0; i < 240 && launchSpeed == 0.f; ++i)
    {
        // Systems first, then the step — the order OnFixedUpdate uses, so the
        // velocity the system writes is the one the next step simulates.
        SystemContext ctx{world, kStep, /*simTick=*/0, nullptr, &actions, events, true, &worlds};

        const std::span<const Assisi::Physics::Contact> contacts = world.physics.Contacts();
        for (const Assisi::Physics::Contact &contact : contacts)
        {
            if (contact.entity == ball)
                impactSpeed = -contact.velocity.y;
        }

        BounceSystem(ctx);

        if (impactSpeed > 0.f)
        {
            const Assisi::Physics::RigidBody *body = world.scene.Get<Assisi::Physics::RigidBody>(ball);
            REQUIRE(body != nullptr);
            launchSpeed = world.physics.GetBodyVelocity(*body).first.y;
        }

        world.physics.Update(kStep);
        world.physics.CaptureState();
    }

    REQUIRE(impactSpeed > 4.f);       // it really did arrive at speed
    CHECK(launchSpeed > 0.f);         // ...and left going the other way
    CHECK(launchSpeed == doctest::Approx(impactSpeed * 0.5f).epsilon(0.01)); // at half of it
}

TEST_CASE("rebound of zero stops a body dead, and a negative one is clamped to that")
{
    // 0 and "negative" are the two ends the component's contract names. A level
    // file is just text, so the negative case has to be handled in the system, not
    // only by the inspector's floor.
    for (const float rebound : {0.f, -2.f})
    {
        WorldManager worlds;
        World       &world = worlds.Create("Dead");
        world.physics.SetContactReporting(true);

        const Assisi::ECS::Entity ball = BuildDropScene(world, {0.f, 3.f, 0.f});
        (void)world.scene.Add<Assisi::Physics::Bounce>(ball, Assisi::Physics::Bounce{.rebound = rebound});

        Assisi::Core::EventQueue  events;
        Assisi::Window::ActionMap actions;

        bool  bounced      = false;
        float afterContact = 1.f;

        for (int32_t i = 0; i < 240 && !bounced; ++i)
        {
            SystemContext ctx{world, kStep, /*simTick=*/0, nullptr, &actions, events, true, &worlds};

            const bool hadContact = !world.physics.Contacts().empty();
            BounceSystem(ctx);

            if (hadContact)
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
    World       &world = worlds.Create("Jittery");
    world.physics.SetContactReporting(true);

    // Floor top is at y = 0.25 and the box's half-extent is 0.5, so this is its
    // resting height: it is already touching, with zero velocity.
    const Assisi::ECS::Entity ball = BuildDropScene(world, {0.f, 0.75f, 0.f});
    (void)world.scene.Add<Assisi::Physics::Bounce>(ball, Assisi::Physics::Bounce{.rebound = 2.f});

    Assisi::Core::EventQueue  events;
    Assisi::Window::ActionMap actions;

    float highest = 0.f;
    for (int32_t i = 0; i < 600; ++i) // ten seconds of lying still
    {
        SystemContext ctx{world, kStep, /*simTick=*/0, nullptr, &actions, events, true, &worlds};
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
    World       &world = worlds.Create("Runaway");
    world.physics.SetContactReporting(true);

    const Assisi::ECS::Entity ball = BuildDropScene(world, {0.f, 0.84f, 0.f});
    (void)world.scene.Add<Assisi::Physics::Bounce>(ball, Assisi::Physics::Bounce{.rebound = 1.5f});

    Assisi::Core::EventQueue  events;
    Assisi::Window::ActionMap actions;

    float firstClosingSpeed = 0.f;
    float highest           = 0.f;

    for (int32_t i = 0; i < 600; ++i) // ten seconds — long enough for a runaway to be obvious
    {
        SystemContext ctx{world, kStep, /*simTick=*/0, nullptr, &actions, events, true, &worlds};

        for (const Assisi::Physics::Contact &contact : world.physics.Contacts())
        {
            if (contact.entity == ball && firstClosingSpeed == 0.f)
                firstClosingSpeed = -glm::dot(contact.velocity, contact.normal);
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
    World       &world = worlds.Create("Hot");
    world.physics.SetContactReporting(true);

    const Assisi::ECS::Entity ball = BuildDropScene(world, {0.f, 3.f, 0.f});
    (void)world.scene.Add<Assisi::Physics::Bounce>(ball, Assisi::Physics::Bounce{.rebound = 1.5f});

    Assisi::Core::EventQueue  events;
    Assisi::Window::ActionMap actions;

    float impactSpeed = 0.f;
    float launchSpeed = 0.f;

    for (int32_t i = 0; i < 240 && launchSpeed == 0.f; ++i)
    {
        SystemContext ctx{world, kStep, /*simTick=*/0, nullptr, &actions, events, true, &worlds};

        for (const Assisi::Physics::Contact &contact : world.physics.Contacts())
        {
            if (contact.entity == ball)
                impactSpeed = -contact.velocity.y;
        }

        BounceSystem(ctx);

        if (impactSpeed > 0.f)
        {
            const Assisi::Physics::RigidBody *body = world.scene.Get<Assisi::Physics::RigidBody>(ball);
            REQUIRE(body != nullptr);
            launchSpeed = world.physics.GetBodyVelocity(*body).first.y;
        }

        world.physics.Update(kStep);
        world.physics.CaptureState();
    }

    REQUIRE(impactSpeed > 4.f);
    CHECK(launchSpeed > impactSpeed); // it left faster than it arrived
    CHECK(launchSpeed == doctest::Approx(impactSpeed * 1.5f).epsilon(0.01));
}

TEST_CASE("A body with no Bounce component is left alone")
{
    // The system's gate is the component, not the contact: everything else in a
    // world collides too, and none of it should be relaunched.
    WorldManager worlds;
    World       &world = worlds.Create("Inert");
    world.physics.SetContactReporting(true);

    const Assisi::ECS::Entity ball = BuildDropScene(world, {0.f, 3.f, 0.f});

    Assisi::Core::EventQueue  events;
    Assisi::Window::ActionMap actions;

    float highestAfterLanding = -100.f;
    bool  landed              = false;

    for (int32_t i = 0; i < 240; ++i)
    {
        SystemContext ctx{world, kStep, /*simTick=*/0, nullptr, &actions, events, true, &worlds};
        if (!world.physics.Contacts().empty())
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
