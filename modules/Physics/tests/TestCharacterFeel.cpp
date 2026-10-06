/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCharacterFeel.cpp
/// @brief The numbers on CharacterDescriptor that decide how a character feels:
///        jumping, friction, the two accelerations, the air cap, the bunny-hop
///        policy, the eye height, gravity scale, and riding a platform.
///
/// Every case here names one field and fails when that field is ignored. That is
/// the point of the file: the mechanics in TestCharacter.cpp still pass if every
/// one of these values is quietly dropped on the floor, because a character that
/// walks and collides correctly at the wrong speed collides just as correctly.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

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

/// A static box, reconciled into @p world at once.
ECS::Entity SpawnFloor(ECS::Scene &scene, Physics::PhysicsWorld &world, glm::vec3 at = {0.f, -0.5f, 0.f},
                       glm::vec3 halfExtents = {20.f, 0.5f, 20.f})
{
    const ECS::Entity entity = PhysicsTests::AddBody(scene, at, PhysicsTests::Box(halfExtents, true));
    world.Reconcile();
    return entity;
}

ECS::Entity SpawnCharacter(ECS::Scene &scene, Physics::PhysicsWorld &world, glm::vec3 at,
                           const Physics::Character &descriptor)
{
    const ECS::Entity entity = PhysicsTests::AddCharacter(scene, at, descriptor);
    world.Reconcile();
    REQUIRE(world.HasBody(entity));
    return entity;
}

void Step(ECS::Scene &scene, Physics::PhysicsWorld &world, ECS::Entity character, glm::vec3 move, bool jump,
          float deltaTime = kStep)
{
    PhysicsTests::Drive(scene, character, move, jump);
    world.Update(deltaTime);
}

/// Speed across the floor, whichever way it is heading.
float HorizontalSpeed(const ECS::Scene &scene, ECS::Entity character)
{
    const glm::vec3 velocity = PhysicsTests::StateOf(scene, character).velocity;
    return glm::length(glm::vec2(velocity.x, velocity.z));
}

void Settle(ECS::Scene &scene, Physics::PhysicsWorld &world, ECS::Entity character, int32_t steps = 120)
{
    for (int32_t i = 0; i < steps; ++i)
    {
        Step(scene, world, character, glm::vec3(0.f), /*jump=*/ false);
    }
}

glm::vec3 CharacterPosition(ECS::Scene &scene)
{
    glm::vec3 position{0.f};
    for (auto [entity, transform, character] : scene.Query<ECS::Transform, Physics::Character>())
    {
        (void)entity;
        (void)character;
        position = transform.position;
    }
    return position;
}

} // namespace

TEST_CASE("Jumping from the ground leaves it, and jumping again in the air does not")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    (void)SpawnFloor(scene, world);

    Physics::Character descriptor{};
    descriptor.coyoteTime     = 0.f; // the plain case: ground only
    descriptor.jumpBufferTime = 0.f;

    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
    Settle(scene, world, character);
    REQUIRE(PhysicsTests::StateOf(scene, character).ground == Physics::GroundState::OnGround);

    Step(scene, world, character, glm::vec3(0.f), /*jump=*/ true);

    const Physics::CharacterState airborne = PhysicsTests::StateOf(scene, character);
    CHECK(airborne.velocity.y > 1.f);
    CHECK(airborne.ground != Physics::GroundState::OnGround);

    // A second request while genuinely in the air changes nothing: without the
    // once-per-flight guard the jump would re-fire and the character would climb.
    const float risingSpeed = PhysicsTests::StateOf(scene, character).velocity.y;
    Step(scene, world, character, glm::vec3(0.f), /*jump=*/ true);
    CHECK(PhysicsTests::StateOf(scene, character).velocity.y < risingSpeed);
}

TEST_CASE("Coyote time lets a jump fire just after walking off a ledge")
{
    // Without it a jump pressed a frame late simply does nothing, which reads as
    // the button being dropped rather than as a rule.
    const auto jumpedAfterLeaving = [](float coyoteTime, int32_t stepsInAir)
    {
        ECS::Scene scene;
        Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

        // A ledge ending at x = 0, with nothing beyond it.
        (void)SpawnFloor(scene, world, {-2.f, -0.5f, 0.f}, {2.f, 0.5f, 5.f});

        Physics::Character descriptor{};
        descriptor.coyoteTime         = coyoteTime;
        descriptor.jumpBufferTime     = 0.f;
        descriptor.groundAcceleration = 1000.f; // reach walking speed at once
        descriptor.walkSpeed          = 4.f;

        const ECS::Entity character = SpawnCharacter(scene, world, {-2.f, 0.f, 0.f}, descriptor);
        Settle(scene, world, character, 60);

        // Walk off the end.
        while (PhysicsTests::StateOf(scene, character).ground == Physics::GroundState::OnGround)
        {
            Step(scene, world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ false);
        }

        for (int32_t i = 0; i < stepsInAir; ++i)
        {
            Step(scene, world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ false);
        }

        const float before = PhysicsTests::StateOf(scene, character).velocity.y;
        Step(scene, world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ true);
        return PhysicsTests::StateOf(scene, character).velocity.y > before + 1.f;
    };

    // Two steps past the edge is ~33 ms, inside a 100 ms window.
    CHECK(jumpedAfterLeaving(0.1f, 2));

    // The same two steps with no window at all: nothing happens.
    CHECK_FALSE(jumpedAfterLeaving(0.f, 2));
}

TEST_CASE("A jump asked for just before landing fires on the landing step")
{
    // The other half of the same complaint: pressed a frame early, and swallowed.
    const auto landedJumping = [](float bufferTime)
    {
        ECS::Scene scene;
        Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
        (void)SpawnFloor(scene, world);

        Physics::Character descriptor{};
        descriptor.coyoteTime     = 0.f;
        descriptor.jumpBufferTime = bufferTime;

        // Dropped from just above the floor, so it lands well inside the buffer
        // below — a longer fall would expire the request and prove nothing.
        const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.1f, 0.f}, descriptor);

        // Ask once, while still falling, then stop asking.
        Step(scene, world, character, glm::vec3(0.f), /*jump=*/ true);
        REQUIRE(PhysicsTests::StateOf(scene, character).ground != Physics::GroundState::OnGround);

        for (int32_t i = 0; i < 30; ++i)
        {
            Step(scene, world, character, glm::vec3(0.f), /*jump=*/ false);
            if (PhysicsTests::StateOf(scene, character).velocity.y > 1.f)
            {
                return true; // the buffered request fired on landing
            }
        }
        return false;
    };

    CHECK(landedJumping(0.2f));

    // With no buffer the early press is simply lost.
    CHECK_FALSE(landedJumping(0.f));
}

TEST_CASE("airAcceleration decides whether a jump can be steered")
{
    // Zero keeps the launch velocity; a large value takes all of it away within
    // a step of asking for the opposite direction, and leaves the character
    // drifting backwards at the air cap.
    const auto driftAfterJump = [](float airAcceleration)
    {
        ECS::Scene scene;
        Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
        (void)SpawnFloor(scene, world);

        Physics::Character descriptor{};
        descriptor.airAcceleration    = airAcceleration;
        descriptor.groundAcceleration = 1000.f;
        descriptor.walkSpeed          = 5.f;
        descriptor.jumpSpeed          = 8.f;
        descriptor.coyoteTime         = 0.f;
        descriptor.jumpBufferTime     = 0.f;

        const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
        Settle(scene, world, character, 60);

        // Build up speed along +x, then jump and ask for the opposite direction.
        for (int32_t i = 0; i < 30; ++i)
        {
            Step(scene, world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ false);
        }
        Step(scene, world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ true);

        for (int32_t i = 0; i < 30; ++i)
        {
            Step(scene, world, character, {-descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ false);
        }
        return PhysicsTests::StateOf(scene, character).velocity.x;
    };

    // No air control: still travelling the way it launched.
    CHECK(driftAfterJump(0.f) > 1.f);

    // Plenty of air control: reversed despite being airborne the whole time.
    CHECK(driftAfterJump(100.f) < 0.f);
}

TEST_CASE("groundAcceleration decides how quickly walking speed is reached")
{
    const auto speedAfterOneStep = [](float groundAcceleration)
    {
        ECS::Scene scene;
        Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
        (void)SpawnFloor(scene, world);

        Physics::Character descriptor{};
        descriptor.groundAcceleration = groundAcceleration;
        descriptor.walkSpeed          = 10.f;

        const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
        Settle(scene, world, character, 60);

        Step(scene, world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ false);
        return PhysicsTests::StateOf(scene, character).velocity.x;
    };

    // The rate is a fraction of the requested speed per second: at 5, one step
    // of 1/60 s gains a twelfth of it.
    CHECK(speedAfterOneStep(5.f) == doctest::Approx(10.f * 5.f * kStep).epsilon(0.05));

    // A rate that would overshoot in one step stops at the requested speed.
    CHECK(speedAfterOneStep(1000.f) == doctest::Approx(10.f).epsilon(0.05));
}

TEST_CASE("With the defaults a character reaches its walk speed in about an eighth of a second")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    (void)SpawnFloor(scene, world);

    const Physics::Character descriptor{};
    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
    Settle(scene, world, character, 60);

    const glm::vec3 wish{descriptor.walkSpeed, 0.f, 0.f};

    // Not instant: two steps in, less than half way.
    Step(scene, world, character, wish, /*jump=*/ false);
    Step(scene, world, character, wish, /*jump=*/ false);
    CHECK(HorizontalSpeed(scene, character) < 0.5f * descriptor.walkSpeed);

    // Friction works against the gain on every step after the first, which is
    // why this takes eight steps and not the six the rate alone would give.
    for (int32_t i = 0; i < 8; ++i)
    {
        Step(scene, world, character, wish, /*jump=*/ false);
    }
    CHECK(HorizontalSpeed(scene, character) == doctest::Approx(descriptor.walkSpeed).epsilon(0.01));
}

TEST_CASE("Friction stops a character from walk speed in under half a second, and firmly")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    (void)SpawnFloor(scene, world);

    const Physics::Character descriptor{};
    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
    Settle(scene, world, character, 60);

    for (int32_t i = 0; i < 60; ++i)
    {
        Step(scene, world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ false);
    }
    REQUIRE(HorizontalSpeed(scene, character) == doctest::Approx(descriptor.walkSpeed).epsilon(0.01));

    // A fifth of a second after letting go it is still sliding.
    for (int32_t i = 0; i < 12; ++i)
    {
        Step(scene, world, character, glm::vec3(0.f), /*jump=*/ false);
    }
    CHECK(HorizontalSpeed(scene, character) > 0.5f);

    // By half a second it is at rest. Friction with no stopSpeed would still be
    // creeping at half a metre a second here.
    for (int32_t i = 0; i < 18; ++i)
    {
        Step(scene, world, character, glm::vec3(0.f), /*jump=*/ false);
    }
    CHECK(HorizontalSpeed(scene, character) < 0.001f);
}

TEST_CASE("Holding one direction in the air never passes the air cap")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

    // No floor and no gravity: airborne for as long as the case runs.
    Physics::Character descriptor{};
    descriptor.gravityScale = 0.f;
    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);

    float fastest = 0.f;
    for (int32_t i = 0; i < 120; ++i)
    {
        Step(scene, world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ false);
        fastest = glm::max(fastest, HorizontalSpeed(scene, character));
    }

    CHECK(fastest <= descriptor.airWishSpeedCap + 0.001f);
    CHECK(fastest > 0.9f * descriptor.airWishSpeedCap); // it did steer
}

TEST_CASE("Turning the requested direction in the air gains speed past the air cap")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

    Physics::Character descriptor{};
    descriptor.gravityScale = 0.f;
    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);

    // A quarter turn a second, which is a slow sweep of the mouse.
    constexpr float kTurnRadiansPerSecond = 1.5708f;

    for (int32_t i = 0; i < 120; ++i)
    {
        const float angle = kTurnRadiansPerSecond * kStep * static_cast<float>(i);
        const glm::vec3 wish = glm::vec3(std::cos(angle), 0.f, std::sin(angle)) * descriptor.walkSpeed;
        Step(scene, world, character, wish, /*jump=*/ false);
    }

    CHECK(HorizontalSpeed(scene, character) > 2.f * descriptor.airWishSpeedCap);
}

TEST_CASE("The bunny-hop policy decides how much speed a jump leaves the ground with")
{
    static constexpr float kWalkSpeed  = 4.f;
    static constexpr float kRunUpSpeed = 10.f;

    const auto takeOffSpeed = [](Physics::BunnyHopPolicy policy)
    {
        ECS::Scene scene;
        Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
        (void)SpawnFloor(scene, world);

        Physics::Character descriptor{};
        descriptor.walkSpeed = kWalkSpeed;
        descriptor.groundAcceleration = 1000.f;
        descriptor.bunnyHop = policy;
        descriptor.bunnyHopSpeedCap = 1.7f;
        descriptor.coyoteTime = 0.f;
        descriptor.jumpBufferTime = 0.f;

        const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
        Settle(scene, world, character, 60);

        // Faster than the walk speed, standing in for speed carried into a
        // landing: the request's own length is the speed asked for.
        for (int32_t i = 0; i < 30; ++i)
        {
            Step(scene, world, character, {kRunUpSpeed, 0.f, 0.f}, /*jump=*/ false);
        }
        REQUIRE(HorizontalSpeed(scene, character) == doctest::Approx(kRunUpSpeed).epsilon(0.01));

        Step(scene, world, character, {kRunUpSpeed, 0.f, 0.f}, /*jump=*/ true);
        REQUIRE(PhysicsTests::StateOf(scene, character).velocity.y > 1.f);
        return HorizontalSpeed(scene, character);
    };

    // Nothing lost, not even the step of friction a grounded character pays.
    CHECK(takeOffSpeed(Physics::BunnyHopPolicy::Allow) == doctest::Approx(kRunUpSpeed).epsilon(0.01));

    CHECK(takeOffSpeed(Physics::BunnyHopPolicy::Cap) == doctest::Approx(1.7f * kWalkSpeed).epsilon(0.01));

    CHECK(takeOffSpeed(Physics::BunnyHopPolicy::Disallow) == doctest::Approx(kWalkSpeed).epsilon(0.01));
}

TEST_CASE("Under the Boost policy a jump slows a character facing its travel and speeds one facing away")
{
    static constexpr float kWalkSpeed  = 4.f;
    static constexpr float kRunUpSpeed = 10.f;

    // A standing jump may leave the ground at half as much again as walkSpeed.
    static constexpr float kLimit = 1.5f * kWalkSpeed;

    const auto takeOffSpeed = [](glm::vec3 facing)
    {
        ECS::Scene scene;
        Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
        (void)SpawnFloor(scene, world);

        Physics::Character descriptor{};
        descriptor.walkSpeed          = kWalkSpeed;
        descriptor.groundAcceleration = 1000.f;
        descriptor.bunnyHop           = Physics::BunnyHopPolicy::Boost;
        descriptor.coyoteTime         = 0.f;
        descriptor.jumpBufferTime     = 0.f;

        const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
        Settle(scene, world, character, 60);
        PhysicsTests::Face(scene, character, facing);

        // Travelling along +x, well over the limit.
        for (int32_t i = 0; i < 30; ++i)
        {
            Step(scene, world, character, {kRunUpSpeed, 0.f, 0.f}, /*jump=*/ false);
        }
        REQUIRE(HorizontalSpeed(scene, character) == doctest::Approx(kRunUpSpeed).epsilon(0.01));

        // No direction held, as a back hop is done.
        Step(scene, world, character, glm::vec3(0.f), /*jump=*/ true);
        REQUIRE(PhysicsTests::StateOf(scene, character).velocity.y > 1.f);
        return PhysicsTests::StateOf(scene, character).velocity.x;
    };

    // Looking where it is going: the excess over the limit comes off.
    CHECK(takeOffSpeed({1.f, 0.f, 0.f}) == doctest::Approx(kLimit).epsilon(0.01));

    // Looking back the way it came: the same excess is taken off along the
    // facing, which is added to the travel.
    CHECK(takeOffSpeed({-1.f, 0.f, 0.f}) == doctest::Approx(kRunUpSpeed + (kRunUpSpeed - kLimit)).epsilon(0.01));
}

TEST_CASE("The same input covers the same ground at 60 and at 120 steps a second")
{
    const auto distanceWalked = [](int32_t stepsPerSecond)
    {
        ECS::Scene scene;
        Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
        (void)SpawnFloor(scene, world);

        const Physics::Character descriptor{};
        const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);

        const float deltaTime = 1.f / static_cast<float>(stepsPerSecond);
        for (int32_t i = 0; i < stepsPerSecond; ++i)
        {
            Step(scene, world, character, glm::vec3(0.f), /*jump=*/ false, deltaTime);
        }

        // A second of walking, then a second of letting friction stop it.
        for (int32_t i = 0; i < stepsPerSecond; ++i)
        {
            Step(scene, world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ false, deltaTime);
        }
        for (int32_t i = 0; i < stepsPerSecond; ++i)
        {
            Step(scene, world, character, glm::vec3(0.f), /*jump=*/ false, deltaTime);
        }
        return CharacterPosition(scene).x;
    };

    const float at60  = distanceWalked(60);
    const float at120 = distanceWalked(120);

    CHECK(at60 > 3.f); // it walked
    // Within a tenth of a metre: what one step's worth of speed covers.
    CHECK(at120 == doctest::Approx(at60).epsilon(0.03));

    // The distance is mostly time spent at full speed, which hides the ramp. A
    // twentieth of a second from rest is all ramp: a gain that ignored the
    // step length would be twice as far along at the faster rate.
    const auto speedPartWayUp = [](int32_t stepsPerSecond)
    {
        ECS::Scene scene;
        Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
        (void)SpawnFloor(scene, world);

        const Physics::Character descriptor{};
        const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);

        const float deltaTime = 1.f / static_cast<float>(stepsPerSecond);
        for (int32_t i = 0; i < stepsPerSecond; ++i)
        {
            Step(scene, world, character, glm::vec3(0.f), /*jump=*/ false, deltaTime);
        }
        for (int32_t i = 0; i < stepsPerSecond / 20; ++i)
        {
            Step(scene, world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ false, deltaTime);
        }
        return HorizontalSpeed(scene, character);
    };

    CHECK(speedPartWayUp(120) == doctest::Approx(speedPartWayUp(60)).epsilon(0.1));
}

TEST_CASE("The eye eases down when crouching on the ground")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    (void)SpawnFloor(scene, world);

    const Physics::Character descriptor{};
    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
    Settle(scene, world, character, 60);
    REQUIRE(PhysicsTests::StateOf(scene, character).eyeHeight == doctest::Approx(descriptor.eyeHeight));

    REQUIRE(PhysicsTests::AskStance(scene, world, character, Physics::Stance::Crouching));
    Step(scene, world, character, glm::vec3(0.f), /*jump=*/ false);

    // One step in: on its way, not there. A snap would read as a camera cut.
    const float partway = PhysicsTests::StateOf(scene, character).eyeHeight;
    CHECK(partway < descriptor.eyeHeight);
    CHECK(partway > descriptor.crouchEyeHeight);

    Settle(scene, world, character, 30);
    CHECK(PhysicsTests::StateOf(scene, character).eyeHeight == doctest::Approx(descriptor.crouchEyeHeight));
}

TEST_CASE("gravityScale changes how fast a character falls")
{
    const auto fallAfter = [](float gravityScale, int32_t steps)
    {
        ECS::Scene scene;
        Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

        Physics::Character descriptor{};
        descriptor.gravityScale = gravityScale;

        const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
        Settle(scene, world, character, steps);
        return CharacterPosition(scene).y;
    };

    const float normal = fallAfter(1.f, 30);
    const float heavy  = fallAfter(2.f, 30);
    const float held   = fallAfter(0.f, 30);

    CHECK(heavy < normal);           // twice the pull, further down
    CHECK(held == doctest::Approx(0.f).epsilon(0.01)); // no pull, no fall
}
