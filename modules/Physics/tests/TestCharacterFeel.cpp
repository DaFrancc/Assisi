/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCharacterFeel.cpp
/// @brief The numbers on CharacterDescriptor that decide how a character feels:
///        jumping, the two accelerations, gravity scale, and riding a platform.
///
/// Every case here names one field and fails when that field is ignored. That is
/// the point of the file: the mechanics in TestCharacter.cpp still pass if every
/// one of these values is quietly dropped on the floor, because a character that
/// walks and collides correctly at the wrong speed collides just as correctly.

#include <doctest/doctest.h>

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

void Step(Physics::PhysicsWorld &world, const Physics::Character &character, glm::vec3 move, bool jump)
{
    world.MoveCharacter(character, move, jump);
    world.Update(kStep);
    world.CaptureState();
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
    // Zero keeps the launch velocity; a large value turns the character around in
    // flight. A build that applied the ground rate in the air would pass neither.
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

    // A gentle ramp is nowhere near top speed after one step of 1/60 s.
    CHECK(speedAfterOneStep(5.f) < 2.f);

    // A steep one is there immediately, which is what the default is tuned to do.
    CHECK(speedAfterOneStep(1000.f) == doctest::Approx(10.f).epsilon(0.05));
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
