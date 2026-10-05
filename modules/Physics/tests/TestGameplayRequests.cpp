/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestGameplayRequests.cpp
/// @brief Forces, impulses, torque, Wake and Sleep, and the facts gameplay reads
/// off a body.
///
/// Most cases run without gravity and without damping, so a body's velocity
/// after a step is exactly what the request gave it and the arithmetic in the
/// checks is the arithmetic in the header.

#include <doctest/doctest.h>

#include <cstdint>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include "PhysicsTestScene.hpp"

using namespace Assisi;
using Assisi::PhysicsTests::kStep;
using Assisi::PhysicsTests::Step;

namespace
{

constexpr float kMass = 2.f;
constexpr float kRadius = 0.5f;

/// A solid sphere's moment of inertia about any axis through its centre.
constexpr float kInertia = 0.4f * kMass * kRadius * kRadius;

/// A test scene with no gravity.
struct Weightless : PhysicsTests::TestScene
{
    Weightless() { world.SetGravity(glm::vec3(0.f)); }
};

/// A ball of kMass that nothing slows down.
ECS::Entity AddFreeBall(ECS::Scene &scene, glm::vec3 position)
{
    PhysicsTests::BodySpec spec = PhysicsTests::Ball(kRadius, /*isStatic=*/ false);
    spec.rigidBody->mass = kMass;
    spec.rigidBody->linearDamping = 0.f;
    spec.rigidBody->angularDamping = 0.f;
    return PhysicsTests::AddBody(scene, position, spec);
}

/// A ball with no damping, already built.
ECS::Entity BuiltFreeBall(PhysicsTests::TestScene &test, glm::vec3 position)
{
    const ECS::Entity ball = AddFreeBall(test.scene, position);
    test.world.Reconcile();
    return ball;
}

Physics::BodyState StateOf(const PhysicsTests::TestScene &test, ECS::Entity entity)
{
    const Physics::BodyState *state = test.scene.Get<Physics::BodyState>(entity);
    REQUIRE(state != nullptr);
    return *state;
}

} // namespace

TEST_CASE("An impulse changes a body's velocity by itself over the mass, once")
{
    Weightless test;
    const ECS::Entity ball = BuiltFreeBall(test, {0.f, 0.f, 0.f});

    test.world.AddImpulse(ball, {4.f, 0.f, 0.f});
    Step(test.world);
    CHECK(StateOf(test, ball).linearVelocity.x == doctest::Approx(4.f / kMass));

    Step(test.world);
    CHECK(StateOf(test, ball).linearVelocity.x == doctest::Approx(4.f / kMass));
}

TEST_CASE("A force acts for the one step it is applied to")
{
    Weightless test;
    const ECS::Entity ball = BuiltFreeBall(test, {0.f, 0.f, 0.f});

    constexpr float kForce = 120.f;
    test.world.AddForce(ball, {kForce, 0.f, 0.f});
    Step(test.world);
    const float pushed = kForce * kStep / kMass;
    CHECK(StateOf(test, ball).linearVelocity.x == doctest::Approx(pushed));

    // Not asked for again, so not applied again.
    Step(test.world);
    CHECK(StateOf(test, ball).linearVelocity.x == doctest::Approx(pushed));
}

TEST_CASE("A push away from the centre spins the body as well")
{
    Weightless test;
    const ECS::Entity forced = BuiltFreeBall(test, {0.f, 0.f, 0.f});
    const ECS::Entity kicked = BuiltFreeBall(test, {10.f, 0.f, 0.f});

    // Along +X at the top of each ball: it turns about -Z.
    test.world.AddForceAt(forced, {60.f, 0.f, 0.f}, {0.f, kRadius, 0.f});
    test.world.AddImpulseAt(kicked, {1.f, 0.f, 0.f}, {10.f, kRadius, 0.f});
    Step(test.world);

    CHECK(StateOf(test, forced).linearVelocity.x > 0.f);
    CHECK(StateOf(test, forced).angularVelocity.z < 0.f);
    CHECK(StateOf(test, kicked).linearVelocity.x == doctest::Approx(1.f / kMass));
    CHECK(StateOf(test, kicked).angularVelocity.z == doctest::Approx(-kRadius / kInertia).epsilon(0.02));
}

TEST_CASE("Torque and angular impulses spin a body without moving it")
{
    Weightless test;
    const ECS::Entity twisted = BuiltFreeBall(test, {0.f, 0.f, 0.f});
    const ECS::Entity spun = BuiltFreeBall(test, {10.f, 0.f, 0.f});

    constexpr float kTorque = 12.f;
    test.world.AddTorque(twisted, {0.f, kTorque, 0.f});
    test.world.AddAngularImpulse(spun, {0.f, kInertia, 0.f});
    Step(test.world);

    CHECK(StateOf(test, twisted).angularVelocity.y == doctest::Approx(kTorque * kStep / kInertia).epsilon(0.02));
    CHECK(StateOf(test, spun).angularVelocity.y == doctest::Approx(1.f).epsilon(0.02));
    CHECK(StateOf(test, twisted).linearVelocity == glm::vec3(0.f));
    CHECK(StateOf(test, spun).linearVelocity == glm::vec3(0.f));
}

TEST_CASE("A request made before the body exists reaches it on the step it is built")
{
    // The spawn-and-launch case: a system spawns a projectile and kicks it in the
    // same frame, before any reconcile has built its body.
    Weightless test;
    const ECS::Entity ball = AddFreeBall(test.scene, {0.f, 0.f, 0.f});
    test.world.AddImpulse(ball, {0.f, 0.f, 6.f});

    Step(test.world);
    CHECK(StateOf(test, ball).linearVelocity.z == doctest::Approx(6.f / kMass));
}

TEST_CASE("A reconcile on its own leaves requests for the step")
{
    Weightless test;
    const ECS::Entity ball = BuiltFreeBall(test, {0.f, 0.f, 0.f});

    test.world.AddImpulse(ball, {2.f, 0.f, 0.f});
    test.world.Reconcile();
    CHECK(test.world.GetBodyState(ball).linearVelocity == glm::vec3(0.f));

    Step(test.world);
    CHECK(test.world.GetBodyState(ball).linearVelocity.x == doctest::Approx(2.f / kMass));
}

TEST_CASE("Sleep stops a body where it is, and Wake starts it again")
{
    Weightless test;
    const ECS::Entity ball = BuiltFreeBall(test, {0.f, 0.f, 0.f});
    test.world.AddImpulse(ball, {2.f, 0.f, 0.f});
    Step(test.world);
    REQUIRE(test.world.IsBodyActive(ball));

    test.world.Sleep(ball);
    Step(test.world);
    CHECK_FALSE(test.world.IsBodyActive(ball));
    CHECK(StateOf(test, ball).asleep);

    const glm::vec3 rested = test.scene.Get<ECS::Transform>(ball)->position;
    Step(test.world, 10);
    CHECK(test.scene.Get<ECS::Transform>(ball)->position == rested);

    test.world.Wake(ball);
    Step(test.world);
    CHECK(test.world.IsBodyActive(ball));
    CHECK_FALSE(StateOf(test, ball).asleep);
}

TEST_CASE("Pushes on something that cannot be pushed change nothing")
{
    Weightless test;
    const ECS::Entity wall = PhysicsTests::AddBody(test.scene, {0.f, 0.f, 0.f},
                                                   PhysicsTests::Box({1.f, 1.f, 1.f}, /*isStatic=*/ true));
    PhysicsTests::BodySpec platformSpec = PhysicsTests::Box({1.f, 0.2f, 1.f}, /*isStatic=*/ false);
    platformSpec.rigidBody->motion = Physics::MotionType::Kinematic;
    const ECS::Entity platform = PhysicsTests::AddBody(test.scene, {5.f, 0.f, 0.f}, platformSpec);
    const ECS::Entity nobody = test.scene.Create();

    for (const ECS::Entity entity : {wall, platform, nobody, ECS::NullEntity})
    {
        test.world.AddForce(entity, {100.f, 0.f, 0.f});
        test.world.AddImpulse(entity, {100.f, 0.f, 0.f});
        test.world.AddTorque(entity, {0.f, 100.f, 0.f});
        test.world.AddAngularImpulse(entity, {0.f, 100.f, 0.f});
        test.world.Wake(entity);
        test.world.Sleep(entity);
    }
    Step(test.world);

    CHECK(test.scene.Get<ECS::Transform>(wall)->position == glm::vec3(0.f));
    CHECK(test.scene.Get<ECS::Transform>(platform)->position == glm::vec3(5.f, 0.f, 0.f));
    CHECK(test.world.GetBodyState(platform).linearVelocity == glm::vec3(0.f));
}

TEST_CASE("An impulse knocks a character back, and its movement carries on from there")
{
    PhysicsTests::TestScene test;
    (void)PhysicsTests::AddFloor(test.scene);
    const ECS::Entity character = PhysicsTests::AddCharacter(test.scene, {0.f, 0.f, 0.f});
    Step(test.world, 30);
    REQUIRE(PhysicsTests::StateOf(test.scene, character).ground == Physics::GroundState::OnGround);

    const float mass = test.world.Mass(character);
    REQUIRE(mass > 0.f);
    constexpr float kLaunchSpeed = 6.f;
    test.world.AddImpulse(character, {0.f, kLaunchSpeed * mass, 0.f});
    Step(test.world);

    // Left the floor at the launch speed, less one step of gravity.
    const Physics::CharacterState launched = PhysicsTests::StateOf(test.scene, character);
    CHECK(launched.velocity.y > kLaunchSpeed * 0.9f);
    CHECK(launched.velocity.y < kLaunchSpeed);
    Step(test.world, 10);
    CHECK(test.scene.Get<ECS::Transform>(character)->position.y > 0.5f);
}

TEST_CASE("A force pushes a character for the step it is applied to")
{
    // In the air, with no gravity and nothing asked of it, a character keeps
    // whatever velocity it has, so the push is all that shows.
    Weightless test;
    const ECS::Entity character = PhysicsTests::AddCharacter(test.scene, {0.f, 10.f, 0.f});
    Step(test.world);

    const float mass = test.world.Mass(character);
    constexpr float kForce = 700.f;
    test.world.AddForce(character, {kForce, 0.f, 0.f});
    Step(test.world);
    const float pushed = kForce * kStep / mass;
    CHECK(PhysicsTests::StateOf(test.scene, character).velocity.x == doctest::Approx(pushed));

    Step(test.world);
    CHECK(PhysicsTests::StateOf(test.scene, character).velocity.x == doctest::Approx(pushed));
}

TEST_CASE("Mass is what the simulation uses: as authored, from the volume, or nothing")
{
    PhysicsTests::TestScene test;
    const ECS::Entity authored = BuiltFreeBall(test, {0.f, 0.f, 0.f});
    const ECS::Entity fromVolume =
        PhysicsTests::AddBody(test.scene, {5.f, 0.f, 0.f}, PhysicsTests::Ball(kRadius, /*isStatic=*/ false));
    const ECS::Entity wall =
        PhysicsTests::AddBody(test.scene, {10.f, 0.f, 0.f}, PhysicsTests::Box({1.f, 1.f, 1.f}, /*isStatic=*/ true));
    PhysicsTests::BodySpec platformSpec = PhysicsTests::Box({1.f, 0.2f, 1.f}, /*isStatic=*/ false);
    platformSpec.rigidBody->motion = Physics::MotionType::Kinematic;
    const ECS::Entity platform = PhysicsTests::AddBody(test.scene, {15.f, 0.f, 0.f}, platformSpec);
    Physics::Character heavy;
    heavy.mass = 90.f;
    const ECS::Entity character = PhysicsTests::AddCharacter(test.scene, {20.f, 0.f, 0.f}, heavy);
    test.world.Reconcile();

    CHECK(test.world.Mass(authored) == doctest::Approx(kMass));

    // Water's density over a half-metre sphere's volume.
    constexpr float kWaterDensity = 1000.f;
    const float volume = 4.f / 3.f * glm::pi<float>() * kRadius * kRadius * kRadius;
    CHECK(test.world.Mass(fromVolume) == doctest::Approx(kWaterDensity * volume).epsilon(0.02));

    CHECK(test.world.Mass(wall) == 0.f);
    CHECK(test.world.Mass(platform) == 0.f);
    CHECK(test.world.Mass(character) == doctest::Approx(90.f));
    CHECK(test.world.Mass(test.scene.Create()) == 0.f);
}

TEST_CASE("BodyOf names the entity whose body a piece belongs to")
{
    PhysicsTests::TestScene test;
    const ECS::Entity ball = BuiltFreeBall(test, {0.f, 0.f, 0.f});
    const ECS::Entity bare = test.scene.Create();

    CHECK(test.world.BodyOf(ball) == ball);
    CHECK(test.world.BodyOf(bare) == ECS::NullEntity);
    CHECK(test.world.BodyOf(ECS::NullEntity) == ECS::NullEntity);
}

TEST_CASE("GetBodyState reads the body as it is now")
{
    Weightless test;
    const ECS::Entity ball = BuiltFreeBall(test, {0.f, 0.f, 0.f});
    test.world.AddImpulse(ball, {0.f, 4.f, 0.f});
    test.world.AddAngularImpulse(ball, {kInertia, 0.f, 0.f});
    Step(test.world);

    const Physics::BodyState state = test.world.GetBodyState(ball);
    CHECK(state.linearVelocity.y == doctest::Approx(4.f / kMass));
    CHECK(state.angularVelocity.x == doctest::Approx(1.f).epsilon(0.02));
    CHECK_FALSE(state.asleep);

    const Physics::BodyState none = test.world.GetBodyState(test.scene.Create());
    CHECK(none.linearVelocity == glm::vec3(0.f));
    CHECK(none.angularVelocity == glm::vec3(0.f));
}
