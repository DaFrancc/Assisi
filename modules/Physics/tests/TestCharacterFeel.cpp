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

using namespace Assisi;

namespace
{

constexpr float kStep = 1.f / 60.f;

ECS::Entity SpawnFloor(ECS::Scene &scene, Physics::PhysicsWorld &world, glm::vec3 at = {0.f, -0.5f, 0.f},
                       glm::vec3 halfExtents = {20.f, 0.5f, 20.f}, bool isStatic = true)
{
    const ECS::Entity entity  = scene.Create();
    ECS::Transform   *transform = scene.Add<ECS::Transform>(entity);
    REQUIRE(transform != nullptr);
    transform->position = at;

    Physics::RigidBodyDescriptor descriptor{};
    descriptor.halfExtents = halfExtents;
    descriptor.isStatic    = isStatic;
    REQUIRE(scene.Add<Physics::RigidBodyDescriptor>(entity, descriptor) != nullptr);

    (void)world.AddBodyFromDescriptor(scene, entity, *transform, descriptor);
    return entity;
}

Physics::Character SpawnCharacter(ECS::Scene &scene, Physics::PhysicsWorld &world, glm::vec3 at,
                                  const Physics::CharacterDescriptor &descriptor)
{
    const ECS::Entity entity  = scene.Create();
    ECS::Transform   *transform = scene.Add<ECS::Transform>(entity);
    REQUIRE(transform != nullptr);
    transform->position = at;
    REQUIRE(scene.Add<Physics::CharacterDescriptor>(entity, descriptor) != nullptr);

    const auto added = world.AddCharacterFromDescriptor(scene, entity, *transform, descriptor);
    REQUIRE(added.has_value());
    return *added;
}

void Step(Physics::PhysicsWorld &world, const Physics::Character &character, glm::vec3 move, bool jump,
          float deltaTime = kStep)
{
    world.MoveCharacter(character, move, jump);
    world.Update(deltaTime);
    world.CaptureState();
}

/// Speed across the floor, whichever way it is heading.
float HorizontalSpeed(const Physics::PhysicsWorld &world, const Physics::Character &character)
{
    const glm::vec3 velocity = world.GetCharacterState(character).velocity;
    return glm::length(glm::vec2(velocity.x, velocity.z));
}

void Settle(Physics::PhysicsWorld &world, const Physics::Character &character, int32_t steps = 120)
{
    for (int32_t i = 0; i < steps; ++i)
    {
        Step(world, character, glm::vec3(0.f), /*jump=*/ false);
    }
}

glm::vec3 CharacterPosition(ECS::Scene &scene, Physics::PhysicsWorld &world)
{
    world.InterpolateTransforms(scene, 1.f);
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
    Physics::PhysicsWorld world;
    (void)SpawnFloor(scene, world);

    Physics::CharacterDescriptor descriptor{};
    descriptor.coyoteTime     = 0.f; // the plain case: ground only
    descriptor.jumpBufferTime = 0.f;

    const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
    Settle(world, character);
    REQUIRE(world.GetCharacterState(character).ground == Physics::GroundState::OnGround);

    Step(world, character, glm::vec3(0.f), /*jump=*/ true);

    const Physics::CharacterState airborne = world.GetCharacterState(character);
    CHECK(airborne.velocity.y > 1.f);
    CHECK(airborne.ground != Physics::GroundState::OnGround);

    // A second request while genuinely in the air changes nothing: without the
    // once-per-flight guard the jump would re-fire and the character would climb.
    const float risingSpeed = world.GetCharacterState(character).velocity.y;
    Step(world, character, glm::vec3(0.f), /*jump=*/ true);
    CHECK(world.GetCharacterState(character).velocity.y < risingSpeed);
}

TEST_CASE("Coyote time lets a jump fire just after walking off a ledge")
{
    // Without it a jump pressed a frame late simply does nothing, which reads as
    // the button being dropped rather than as a rule.
    const auto jumpedAfterLeaving = [](float coyoteTime, int32_t stepsInAir)
    {
        ECS::Scene scene;
        Physics::PhysicsWorld world;

        // A ledge ending at x = 0, with nothing beyond it.
        (void)SpawnFloor(scene, world, {-2.f, -0.5f, 0.f}, {2.f, 0.5f, 5.f});

        Physics::CharacterDescriptor descriptor{};
        descriptor.coyoteTime         = coyoteTime;
        descriptor.jumpBufferTime     = 0.f;
        descriptor.groundAcceleration = 1000.f; // reach walking speed at once
        descriptor.walkSpeed          = 4.f;

        const Physics::Character character = SpawnCharacter(scene, world, {-2.f, 0.f, 0.f}, descriptor);
        Settle(world, character, 60);

        // Walk off the end.
        while (world.GetCharacterState(character).ground == Physics::GroundState::OnGround)
        {
            Step(world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ false);
        }

        for (int32_t i = 0; i < stepsInAir; ++i)
        {
            Step(world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ false);
        }

        const float before = world.GetCharacterState(character).velocity.y;
        Step(world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ true);
        return world.GetCharacterState(character).velocity.y > before + 1.f;
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
        Physics::PhysicsWorld world;
        (void)SpawnFloor(scene, world);

        Physics::CharacterDescriptor descriptor{};
        descriptor.coyoteTime     = 0.f;
        descriptor.jumpBufferTime = bufferTime;

        // Dropped from just above the floor, so it lands well inside the buffer
        // below — a longer fall would expire the request and prove nothing.
        const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.1f, 0.f}, descriptor);

        // Ask once, while still falling, then stop asking.
        Step(world, character, glm::vec3(0.f), /*jump=*/ true);
        REQUIRE(world.GetCharacterState(character).ground != Physics::GroundState::OnGround);

        for (int32_t i = 0; i < 30; ++i)
        {
            Step(world, character, glm::vec3(0.f), /*jump=*/ false);
            if (world.GetCharacterState(character).velocity.y > 1.f)
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
        Physics::PhysicsWorld world;
        (void)SpawnFloor(scene, world);

        Physics::CharacterDescriptor descriptor{};
        descriptor.airAcceleration    = airAcceleration;
        descriptor.groundAcceleration = 1000.f;
        descriptor.walkSpeed          = 5.f;
        descriptor.jumpSpeed          = 8.f;
        descriptor.coyoteTime         = 0.f;
        descriptor.jumpBufferTime     = 0.f;

        const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
        Settle(world, character, 60);

        // Build up speed along +x, then jump and ask for the opposite direction.
        for (int32_t i = 0; i < 30; ++i)
        {
            Step(world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ false);
        }
        Step(world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ true);

        for (int32_t i = 0; i < 30; ++i)
        {
            Step(world, character, {-descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ false);
        }
        return world.GetCharacterState(character).velocity.x;
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
        Physics::PhysicsWorld world;
        (void)SpawnFloor(scene, world);

        Physics::CharacterDescriptor descriptor{};
        descriptor.groundAcceleration = groundAcceleration;
        descriptor.walkSpeed          = 10.f;

        const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
        Settle(world, character, 60);

        Step(world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ false);
        return world.GetCharacterState(character).velocity.x;
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
    Physics::PhysicsWorld world;
    (void)SpawnFloor(scene, world);

    const Physics::CharacterDescriptor descriptor{};
    const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
    Settle(world, character, 60);

    const glm::vec3 wish{descriptor.walkSpeed, 0.f, 0.f};

    // Not instant: two steps in, less than half way.
    Step(world, character, wish, /*jump=*/ false);
    Step(world, character, wish, /*jump=*/ false);
    CHECK(HorizontalSpeed(world, character) < 0.5f * descriptor.walkSpeed);

    // Friction works against the gain on every step after the first, which is
    // why this takes eight steps and not the six the rate alone would give.
    for (int32_t i = 0; i < 8; ++i)
    {
        Step(world, character, wish, /*jump=*/ false);
    }
    CHECK(HorizontalSpeed(world, character) == doctest::Approx(descriptor.walkSpeed).epsilon(0.01));
}

TEST_CASE("Friction stops a character from walk speed in under half a second, and firmly")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    (void)SpawnFloor(scene, world);

    const Physics::CharacterDescriptor descriptor{};
    const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
    Settle(world, character, 60);

    for (int32_t i = 0; i < 60; ++i)
    {
        Step(world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ false);
    }
    REQUIRE(HorizontalSpeed(world, character) == doctest::Approx(descriptor.walkSpeed).epsilon(0.01));

    // A fifth of a second after letting go it is still sliding.
    for (int32_t i = 0; i < 12; ++i)
    {
        Step(world, character, glm::vec3(0.f), /*jump=*/ false);
    }
    CHECK(HorizontalSpeed(world, character) > 0.5f);

    // By half a second it is at rest. Friction with no stopSpeed would still be
    // creeping at half a metre a second here.
    for (int32_t i = 0; i < 18; ++i)
    {
        Step(world, character, glm::vec3(0.f), /*jump=*/ false);
    }
    CHECK(HorizontalSpeed(world, character) < 0.001f);
}

TEST_CASE("Holding one direction in the air never passes the air cap")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    // No floor and no gravity: airborne for as long as the case runs.
    Physics::CharacterDescriptor descriptor{};
    descriptor.gravityScale = 0.f;
    const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);

    float fastest = 0.f;
    for (int32_t i = 0; i < 120; ++i)
    {
        Step(world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ false);
        fastest = glm::max(fastest, HorizontalSpeed(world, character));
    }

    CHECK(fastest <= descriptor.airWishSpeedCap + 0.001f);
    CHECK(fastest > 0.9f * descriptor.airWishSpeedCap); // it did steer
}

TEST_CASE("Turning the requested direction in the air gains speed past the air cap")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    Physics::CharacterDescriptor descriptor{};
    descriptor.gravityScale = 0.f;
    const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);

    // A quarter turn a second, which is a slow sweep of the mouse.
    constexpr float kTurnRadiansPerSecond = 1.5708f;

    for (int32_t i = 0; i < 120; ++i)
    {
        const float angle = kTurnRadiansPerSecond * kStep * static_cast<float>(i);
        const glm::vec3 wish = glm::vec3(std::cos(angle), 0.f, std::sin(angle)) * descriptor.walkSpeed;
        Step(world, character, wish, /*jump=*/ false);
    }

    CHECK(HorizontalSpeed(world, character) > 2.f * descriptor.airWishSpeedCap);
}

TEST_CASE("The bunny-hop policy decides how much speed a jump leaves the ground with")
{
    static constexpr float kWalkSpeed  = 4.f;
    static constexpr float kRunUpSpeed = 10.f;

    const auto takeOffSpeed = [](Physics::BunnyHopPolicy policy)
    {
        ECS::Scene scene;
        Physics::PhysicsWorld world;
        (void)SpawnFloor(scene, world);

        Physics::CharacterDescriptor descriptor{};
        descriptor.walkSpeed = kWalkSpeed;
        descriptor.groundAcceleration = 1000.f;
        descriptor.bunnyHop = policy;
        descriptor.bunnyHopSpeedCap = 1.7f;
        descriptor.coyoteTime = 0.f;
        descriptor.jumpBufferTime = 0.f;

        const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
        Settle(world, character, 60);

        // Faster than the walk speed, standing in for speed carried into a
        // landing: the request's own length is the speed asked for.
        for (int32_t i = 0; i < 30; ++i)
        {
            Step(world, character, {kRunUpSpeed, 0.f, 0.f}, /*jump=*/ false);
        }
        REQUIRE(HorizontalSpeed(world, character) == doctest::Approx(kRunUpSpeed).epsilon(0.01));

        Step(world, character, {kRunUpSpeed, 0.f, 0.f}, /*jump=*/ true);
        REQUIRE(world.GetCharacterState(character).velocity.y > 1.f);
        return HorizontalSpeed(world, character);
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
        Physics::PhysicsWorld world;
        (void)SpawnFloor(scene, world);

        Physics::CharacterDescriptor descriptor{};
        descriptor.walkSpeed          = kWalkSpeed;
        descriptor.groundAcceleration = 1000.f;
        descriptor.bunnyHop           = Physics::BunnyHopPolicy::Boost;
        descriptor.coyoteTime         = 0.f;
        descriptor.jumpBufferTime     = 0.f;

        const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
        Settle(world, character, 60);
        world.SetCharacterFacing(character, facing);

        // Travelling along +x, well over the limit.
        for (int32_t i = 0; i < 30; ++i)
        {
            Step(world, character, {kRunUpSpeed, 0.f, 0.f}, /*jump=*/ false);
        }
        REQUIRE(HorizontalSpeed(world, character) == doctest::Approx(kRunUpSpeed).epsilon(0.01));

        // No direction held, as a back hop is done.
        Step(world, character, glm::vec3(0.f), /*jump=*/ true);
        REQUIRE(world.GetCharacterState(character).velocity.y > 1.f);
        return world.GetCharacterState(character).velocity.x;
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
        Physics::PhysicsWorld world;
        (void)SpawnFloor(scene, world);

        const Physics::CharacterDescriptor descriptor{};
        const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);

        const float deltaTime = 1.f / static_cast<float>(stepsPerSecond);
        for (int32_t i = 0; i < stepsPerSecond; ++i)
        {
            Step(world, character, glm::vec3(0.f), /*jump=*/ false, deltaTime);
        }

        // A second of walking, then a second of letting friction stop it.
        for (int32_t i = 0; i < stepsPerSecond; ++i)
        {
            Step(world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ false, deltaTime);
        }
        for (int32_t i = 0; i < stepsPerSecond; ++i)
        {
            Step(world, character, glm::vec3(0.f), /*jump=*/ false, deltaTime);
        }
        return CharacterPosition(scene, world).x;
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
        Physics::PhysicsWorld world;
        (void)SpawnFloor(scene, world);

        const Physics::CharacterDescriptor descriptor{};
        const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);

        const float deltaTime = 1.f / static_cast<float>(stepsPerSecond);
        for (int32_t i = 0; i < stepsPerSecond; ++i)
        {
            Step(world, character, glm::vec3(0.f), /*jump=*/ false, deltaTime);
        }
        for (int32_t i = 0; i < stepsPerSecond / 20; ++i)
        {
            Step(world, character, {descriptor.walkSpeed, 0.f, 0.f}, /*jump=*/ false, deltaTime);
        }
        return HorizontalSpeed(world, character);
    };

    CHECK(speedPartWayUp(120) == doctest::Approx(speedPartWayUp(60)).epsilon(0.1));
}

TEST_CASE("The eye eases down when crouching on the ground")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    (void)SpawnFloor(scene, world);

    const Physics::CharacterDescriptor descriptor{};
    const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
    Settle(world, character, 60);
    REQUIRE(world.GetCharacterState(character).eyeHeight == doctest::Approx(descriptor.eyeHeight));

    REQUIRE(world.SetCharacterStance(character, Physics::Stance::Crouching));
    Step(world, character, glm::vec3(0.f), /*jump=*/ false);

    // One step in: on its way, not there. A snap would read as a camera cut.
    const float partway = world.GetCharacterState(character).eyeHeight;
    CHECK(partway < descriptor.eyeHeight);
    CHECK(partway > descriptor.crouchEyeHeight);

    Settle(world, character, 30);
    CHECK(world.GetCharacterState(character).eyeHeight == doctest::Approx(descriptor.crouchEyeHeight));
}

TEST_CASE("gravityScale changes how fast a character falls")
{
    const auto fallAfter = [](float gravityScale, int32_t steps)
    {
        ECS::Scene scene;
        Physics::PhysicsWorld world;

        Physics::CharacterDescriptor descriptor{};
        descriptor.gravityScale = gravityScale;

        const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
        Settle(world, character, steps);
        return CharacterPosition(scene, world).y;
    };

    const float normal = fallAfter(1.f, 30);
    const float heavy  = fallAfter(2.f, 30);
    const float held   = fallAfter(0.f, 30);

    CHECK(heavy < normal);           // twice the pull, further down
    CHECK(held == doctest::Approx(0.f).epsilon(0.01)); // no pull, no fall
}

TEST_CASE("A character riding a kinematic platform is carried along with it")
{
    // What MoveBodyKinematic exists for. Placing the platform with
    // SetBodyTransform would teleport it with zero velocity, and the character
    // standing on it would be standing on something the simulation believes is
    // still — so it would slide off the back.
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    const ECS::Entity platform  = scene.Create();
    ECS::Transform   *transform = scene.Add<ECS::Transform>(platform);
    REQUIRE(transform != nullptr);
    transform->position = {0.f, -0.5f, 0.f};

    Physics::RigidBodyDescriptor descriptor{};
    descriptor.halfExtents = {4.f, 0.5f, 4.f};
    descriptor.isStatic    = false;
    REQUIRE(scene.Add<Physics::RigidBodyDescriptor>(platform, descriptor) != nullptr);

    const Physics::RigidBody body =
        world.AddBodyFromDescriptor(scene, platform, *transform, descriptor);
    world.SetBodyMotionType(body, Physics::BodyMotion::Kinematic);

    Physics::CharacterDescriptor characterDescriptor{};
    const Physics::Character character =
        SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, characterDescriptor);
    Settle(world, character, 30);

    // Standing on it before anything moves, or the rest of the case is measuring
    // a character in free fall.
    REQUIRE(world.GetCharacterState(character).ground == Physics::GroundState::OnGround);

    // Drive the platform along +x at a steady 1 m/s for two seconds.
    constexpr float kPlatformSpeed = 1.f;

    // Seeded from the body, not from the Transform component: a kinematic body is
    // never written back to its Transform, so driving from the component would
    // aim every step at wherever the body was authored and yank it there.
    glm::vec3 platformAt = world.GetBodyTransform(body).first;
    for (int32_t i = 0; i < 120; ++i)
    {
        platformAt.x += kPlatformSpeed * kStep;
        world.MoveBodyKinematic(body, platformAt, glm::quat{1.f, 0.f, 0.f, 0.f}, kStep);
        Step(world, character, glm::vec3(0.f), /*jump=*/ false);
    }

    // The platform itself actually travelled, or the case proves nothing about
    // the character.
    REQUIRE(world.GetBodyTransform(body).first.x == doctest::Approx(platformAt.x).epsilon(0.1));

    // It is standing on the platform, not on nothing — the rest of the case is
    // meaningless if the ground underfoot is not the thing being moved.
    const Physics::CharacterState state = world.GetCharacterState(character);
    CHECK(state.ground == Physics::GroundState::OnGround);
    CHECK(state.groundEntity == platform);
    CHECK(state.groundVelocity.x == doctest::Approx(kPlatformSpeed).epsilon(0.2));

    // The platform's own Transform followed it. A kinematic body that moves
    // through the simulation and never writes back would be drawn where it was
    // authored — the character would ride away from a platform still sitting at
    // the origin on screen.
    world.InterpolateTransforms(scene, 1.f);
    const ECS::Transform *platformTransform = scene.Get<ECS::Transform>(platform);
    REQUIRE(platformTransform != nullptr);
    CHECK(platformTransform->position.x == doctest::Approx(platformAt.x).epsilon(0.1));

    // It went with the platform rather than being left behind at the origin.
    const glm::vec3 characterAt = CharacterPosition(scene, world);
    CHECK(characterAt.x == doctest::Approx(platformAt.x).epsilon(0.25));
    CHECK(characterAt.x > 1.f);
}
