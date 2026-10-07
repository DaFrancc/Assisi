/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestJoints.cpp
/// @brief Each joint component holds two bodies as its kind says, within its
///        limits and under its motor, until it breaks or either body goes.
///
/// Every case drops or spins a body that would move away freely, and checks the
/// joint kept it. Where a limit is the subject, the same setup with the limit
/// opened is checked to go past it, so the limit assertion is not satisfied by
/// gravity alone.

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
using namespace Assisi::PhysicsTests;

namespace
{

/// Two seconds: long enough for a free body to fall about 20 m, or for a
/// swinging one to reach the bottom of its arc and come back.
constexpr int32_t kSettleSteps = 120;

/// How far a held point may drift (m). The solver holds a joint to within a
/// few millimetres; a centimetre leaves room for the swing's first steps.
constexpr float kHoldTolerance = 0.05f;

/// How far past a hard limit a joint may be read once it rests there (degrees).
constexpr float kAngleTolerance = 2.f;

/// How far past a hard limit an arm arriving at about 3 rad/s carries in the
/// step it arrives (degrees): the solver stops it over a step, not at once.
constexpr float kArrivalTolerance = 5.f;

constexpr float kHalf = 0.25f;
constexpr glm::vec3 kPivot{0.f, 5.f, 0.f};

/// The arm's length from kPivot to the bodies that hang from it.
constexpr float kArm = 1.f;

glm::vec3 PositionOf(const ECS::Scene &scene, ECS::Entity entity)
{
    return scene.Get<ECS::Transform>(entity)->position;
}

/// A small dynamic box at @p position with no damping, so a spin is kept
/// unless something takes it.
ECS::Entity AddCrate(ECS::Scene &scene, glm::vec3 position)
{
    BodySpec spec = Box(glm::vec3(kHalf), /*isStatic=*/ false);
    spec.rigidBody->linearDamping = 0.f;
    spec.rigidBody->angularDamping = 0.f;
    return AddBody(scene, position, spec);
}

/// A crate one arm's length from kPivot along +Z, the arm level.
ECS::Entity AddLevelArm(ECS::Scene &scene)
{
    return AddCrate(scene, kPivot + glm::vec3(0.f, 0.f, kArm));
}

/// Degrees the arm from kPivot to @p entity hangs below level.
float DropDegrees(const ECS::Scene &scene, ECS::Entity entity)
{
    const glm::vec3 arm = PositionOf(scene, entity) - kPivot;
    return glm::degrees(std::asin(glm::clamp(-arm.y / glm::length(arm), -1.f, 1.f)));
}

/// The most the arm drops below level over @p steps.
float MostDrop(TestScene &test, ECS::Entity entity, int32_t steps)
{
    float most = DropDegrees(test.scene, entity);
    for (int32_t i = 0; i < steps; ++i)
    {
        Step(test.world);
        most = std::max(most, DropDegrees(test.scene, entity));
    }
    return most;
}

/// A hinge at kPivot about X, holding a level arm to the world.
Physics::HingeJoint LevelHinge(float limitDegrees)
{
    return Physics::HingeJoint{.anchor = glm::vec3(0.f, 0.f, -kArm),
                               .axis = glm::vec3(1.f, 0.f, 0.f),
                               .minAngle = -limitDegrees,
                               .maxAngle = limitDegrees};
}

/// A test scene with no gravity, for spins and motors.
struct Weightless : TestScene
{
    Weightless() { world.SetGravity(glm::vec3(0.f)); }
};

void SetVelocity(ECS::Scene &scene, ECS::Entity entity, glm::vec3 linear, glm::vec3 angular)
{
    Physics::BodyState &state = *scene.GetMut<Physics::BodyState>(entity);
    state.linearVelocity = linear;
    state.angularVelocity = angular;
}

glm::vec3 SpinOf(const ECS::Scene &scene, ECS::Entity entity)
{
    return scene.Get<Physics::BodyState>(entity)->angularVelocity;
}

/// Degrees @p entity has turned about Y from where it started unturned.
float YawDegrees(const ECS::Scene &scene, ECS::Entity entity)
{
    const glm::vec3 forward = scene.Get<ECS::Transform>(entity)->rotation * glm::vec3(1.f, 0.f, 0.f);
    return glm::degrees(std::atan2(-forward.z, forward.x));
}

/// How many of @p world's last contact events are between @p a and @p b.
int32_t ContactsBetween(const Physics::PhysicsWorld &world, ECS::Entity a, ECS::Entity b)
{
    int32_t count = 0;
    for (const Physics::ContactEvent &event : world.ContactEvents())
    {
        if (event.entity == a && event.other == b)
        {
            ++count;
        }
    }
    return count;
}

/// How far a level arm swings down, at most and once it has come to rest.
struct Drop
{
    float most = 0.f;
    float resting = 0.f;
};

/// The drop of a level arm on a SwingTwistJoint with a cone of @p swing
/// degrees.
Drop DropWithCone(float swing)
{
    TestScene test;
    const ECS::Entity crate = AddLevelArm(test.scene);
    REQUIRE(test.scene.Add(crate, Physics::SwingTwistJoint{.anchor = glm::vec3(0.f, 0.f, -kArm),
                                                           .axis = glm::vec3(0.f, 0.f, 1.f),
                                                           .swingAngle = swing}) != nullptr);
    const float most = MostDrop(test, crate, kSettleSteps);
    return Drop{most, DropDegrees(test.scene, crate)};
}

/// The drop of a level arm on a hinge limited to @p limit degrees.
Drop DropWithLimit(float limit)
{
    TestScene test;
    const ECS::Entity crate = AddLevelArm(test.scene);
    REQUIRE(test.scene.Add(crate, LevelHinge(limit)) != nullptr);
    const float most = MostDrop(test, crate, kSettleSteps);
    return Drop{most, DropDegrees(test.scene, crate)};
}

/// How fast a door on a hinge with @p friction still spins a second after
/// being spun at 3 rad/s.
float SpinAfterASecond(float friction)
{
    Weightless test;
    const ECS::Entity door = AddCrate(test.scene, kPivot);
    REQUIRE(test.scene.Add(door, Physics::HingeJoint{.friction = friction}) != nullptr);
    SetVelocity(test.scene, door, glm::vec3(0.f), glm::vec3(0.f, 3.f, 0.f));
    Step(test.world, kSettleSteps / 2);
    return std::abs(SpinOf(test.scene, door).y);
}

/// What three overlapping crates report on their first step when the first two
/// are joined: contacts between the joined pair, and into @p withThird those
/// between either of them and the third.
int32_t ContactsWhenJoined(bool collideConnected, int32_t &withThird)
{
    Weightless test;
    const ECS::Entity a = AddCrate(test.scene, glm::vec3(0.f));
    const ECS::Entity b = AddCrate(test.scene, glm::vec3(kHalf, 0.f, 0.f));
    const ECS::Entity c = AddCrate(test.scene, glm::vec3(0.f, kHalf, 0.f));
    REQUIRE(test.scene.Add(b, Physics::PointJoint{.other = a, .collideConnected = collideConnected}) != nullptr);
    Step(test.world);
    withThird = ContactsBetween(test.world, a, c) + ContactsBetween(test.world, b, c);
    return ContactsBetween(test.world, a, b);
}

} // namespace

// ---------------------------------------------------------------------------
// Each kind holds
// ---------------------------------------------------------------------------

TEST_CASE("A FixedJoint to the world holds a body where it was built")
{
    TestScene test;
    const ECS::Entity crate = AddCrate(test.scene, kPivot);
    REQUIRE(test.scene.Add(crate, Physics::FixedJoint{}) != nullptr);

    Step(test.world, kSettleSteps);

    CHECK(glm::length(PositionOf(test.scene, crate) - kPivot) < kHoldTolerance);
}

TEST_CASE("A FixedJoint holds two bodies apart that do not touch")
{
    TestScene test;
    const ECS::Entity post = AddBody(test.scene, kPivot, Box(glm::vec3(kHalf), /*isStatic=*/ true));
    const glm::vec3 start = kPivot + glm::vec3(2.f, 0.f, 0.f);
    const ECS::Entity crate = AddCrate(test.scene, start);
    REQUIRE(test.scene.Add(crate, Physics::FixedJoint{.other = post}) != nullptr);

    Step(test.world, kSettleSteps);

    CHECK(glm::length(PositionOf(test.scene, crate) - start) < kHoldTolerance);
}

TEST_CASE("A PointJoint keeps a body at its distance from the pivot while it swings")
{
    TestScene test;
    const ECS::Entity crate = AddLevelArm(test.scene);
    REQUIRE(test.scene.Add(crate, Physics::PointJoint{.anchor = glm::vec3(0.f, 0.f, -kArm)}) != nullptr);

    const float drop = MostDrop(test, crate, kSettleSteps);

    CHECK(std::abs(glm::length(PositionOf(test.scene, crate) - kPivot) - kArm) < kHoldTolerance);
    CHECK(drop > 60.f);
}

TEST_CASE("A HingeJoint turns only about its axis")
{
    TestScene test;
    const ECS::Entity crate = AddLevelArm(test.scene);
    REQUIRE(test.scene.Add(crate, LevelHinge(180.f)) != nullptr);
    // Pushed along the axle and across it: only the swing about the axle is
    // allowed, so it stays in the plane x = 0.
    SetVelocity(test.scene, crate, glm::vec3(3.f, 0.f, 0.f), glm::vec3(0.f, 3.f, 0.f));

    const float drop = MostDrop(test, crate, kSettleSteps);

    CHECK(std::abs(PositionOf(test.scene, crate).x) < kHoldTolerance);
    CHECK(std::abs(glm::length(PositionOf(test.scene, crate) - kPivot) - kArm) < kHoldTolerance);
    CHECK(drop > 60.f);
}

TEST_CASE("A SliderJoint lets a body fall only along its axis, to its limit")
{
    TestScene test;
    const ECS::Entity crate = AddCrate(test.scene, kPivot);
    REQUIRE(test.scene.Add(crate, Physics::SliderJoint{.minDistance = -1.f, .maxDistance = 1.f}) != nullptr);
    SetVelocity(test.scene, crate, glm::vec3(3.f, 0.f, 3.f), glm::vec3(0.f));

    Step(test.world, kSettleSteps);

    const glm::vec3 at = PositionOf(test.scene, crate);
    CHECK(std::abs(at.x) < kHoldTolerance);
    CHECK(std::abs(at.z) < kHoldTolerance);
    CHECK(at.y == doctest::Approx(kPivot.y - 1.f).epsilon(0.01));
}

TEST_CASE("A DistanceJoint lets a body fall until its anchors are maxDistance apart")
{
    TestScene test;
    const ECS::Entity crate = AddCrate(test.scene, kPivot);
    constexpr float kRope = 3.f;
    // The far end is the world, 2 m above and 1 m aside: about 2.2 m away.
    const glm::vec3 hook{1.f, 2.f, 0.f};
    REQUIRE(test.scene.Add(crate, Physics::DistanceJoint{.otherAnchor = hook, .maxDistance = kRope}) != nullptr);

    Step(test.world, kSettleSteps);

    CHECK(glm::length(PositionOf(test.scene, crate) - (kPivot + hook)) == doctest::Approx(kRope).epsilon(0.02));
}

TEST_CASE("A SwingTwistJoint keeps its axis inside its cone")
{
    const Drop limited = DropWithCone(20.f);
    CHECK(limited.most < 20.f + kArrivalTolerance);
    CHECK(limited.resting < 20.f + kAngleTolerance);
    CHECK(DropWithCone(180.f).most > 60.f);
}

// ---------------------------------------------------------------------------
// Limits, friction and motors
// ---------------------------------------------------------------------------

TEST_CASE("A HingeJoint's limits stop its swing")
{
    const Drop limited = DropWithLimit(30.f);
    CHECK(limited.most < 30.f + kArrivalTolerance);
    CHECK(limited.resting < 30.f + kAngleTolerance);
    CHECK(DropWithLimit(180.f).most > 60.f);
}

TEST_CASE("A soft limit gives past its angle and pulls back")
{
    TestScene test;
    const ECS::Entity crate = AddLevelArm(test.scene);
    Physics::HingeJoint hinge = LevelHinge(30.f);
    hinge.limitSpringFrequency = 10.f;
    hinge.limitSpringDamping = 0.2f;
    REQUIRE(test.scene.Add(crate, hinge) != nullptr);

    const float most = MostDrop(test, crate, kSettleSteps);
    Step(test.world, kSettleSteps * 2);

    // At 10 Hz the arm's weight holds it a few degrees past the limit once it
    // settles; the swing that arrives there carries it well beyond, further
    // than a hard limit ever lets it.
    CHECK(most > 30.f + kArrivalTolerance * 2.f);
    CHECK(std::abs(DropDegrees(test.scene, crate) - 30.f) < 5.f);
}

TEST_CASE("Hinge friction stops a spinning door; without it the door keeps spinning")
{
    CHECK(SpinAfterASecond(0.f) == doctest::Approx(3.f).epsilon(0.05));
    CHECK(SpinAfterASecond(50.f) < 0.05f);
}

TEST_CASE("A HingeMotor turns its hinge at its target speed")
{
    Weightless test;
    const ECS::Entity wheel = AddCrate(test.scene, kPivot);
    REQUIRE(test.scene.Add(wheel, Physics::HingeJoint{}) != nullptr);
    REQUIRE(test.scene.Add(wheel, Physics::HingeMotor{.target = 90.f}) != nullptr);

    Step(test.world, kSettleSteps / 4);

    CHECK(std::abs(SpinOf(test.scene, wheel).y) == doctest::Approx(glm::radians(90.f)).epsilon(0.05));
}

TEST_CASE("A HingeMotor in Position mode turns its hinge to the target angle and holds it")
{
    Weightless test;
    const ECS::Entity door = AddCrate(test.scene, kPivot);
    REQUIRE(test.scene.Add(door, Physics::HingeJoint{}) != nullptr);
    REQUIRE(test.scene.Add(door, Physics::HingeMotor{.target = 45.f, .mode = Physics::MotorMode::Position}) !=
            nullptr);

    Step(test.world, kSettleSteps * 2);

    CHECK(std::abs(YawDegrees(test.scene, door)) == doctest::Approx(45.f).epsilon(0.05));
}

TEST_CASE("A SliderMotor slides its body at its target speed")
{
    Weightless test;
    const ECS::Entity lift = AddCrate(test.scene, kPivot);
    REQUIRE(test.scene.Add(lift, Physics::SliderJoint{.minDistance = -10.f, .maxDistance = 10.f}) != nullptr);
    REQUIRE(test.scene.Add(lift, Physics::SliderMotor{.target = 2.f}) != nullptr);

    Step(test.world, kSettleSteps / 4);

    CHECK(std::abs(test.scene.Get<Physics::BodyState>(lift)->linearVelocity.y) ==
          doctest::Approx(2.f).epsilon(0.05));
}

TEST_CASE("Editing a DistanceJoint's range takes effect on the live joint")
{
    TestScene test;
    const ECS::Entity crate = AddCrate(test.scene, kPivot);
    const glm::vec3 hook{0.f, 1.f, 0.f};
    REQUIRE(test.scene.Add(crate, Physics::DistanceJoint{.otherAnchor = hook, .maxDistance = 3.f}) != nullptr);
    Step(test.world, kSettleSteps);
    REQUIRE(glm::length(PositionOf(test.scene, crate) - (kPivot + hook)) == doctest::Approx(3.f).epsilon(0.02));

    test.scene.GetMut<Physics::DistanceJoint>(crate)->maxDistance = 2.f;
    Step(test.world, kSettleSteps);

    CHECK(glm::length(PositionOf(test.scene, crate) - (kPivot + hook)) == doctest::Approx(2.f).epsilon(0.02));
}

// ---------------------------------------------------------------------------
// Breaking
// ---------------------------------------------------------------------------

TEST_CASE("A joint pulled harder than its breakForce breaks once, loses its component, and lets go")
{
    TestScene test;
    const ECS::Entity crate = AddCrate(test.scene, kPivot);
    REQUIRE(test.scene.Add(crate, Physics::FixedJoint{.breakForce = 1.f}) != nullptr);
    // Its weight alone pulls on the joint far harder than 1 N.
    test.world.Reconcile();
    REQUIRE(test.world.Mass(crate) > 1.f);

    int32_t breaks = 0;
    for (int32_t i = 0; i < kSettleSteps / 4; ++i)
    {
        Step(test.world);
        for (const Physics::JointBroke &broke : test.world.BrokenJoints())
        {
            ++breaks;
            CHECK(broke.owner == crate);
            CHECK(broke.other == ECS::NullEntity);
            CHECK(broke.kind == Physics::JointKind::Fixed);
            CHECK(broke.force > 1.f);
        }
    }

    CHECK(breaks == 1);
    CHECK_FALSE(test.scene.Has<Physics::FixedJoint>(crate));
    CHECK(PositionOf(test.scene, crate).y < kPivot.y - 0.5f);
}

TEST_CASE("A joint with no breakForce never breaks")
{
    TestScene test;
    const ECS::Entity crate = AddCrate(test.scene, kPivot);
    REQUIRE(test.scene.Add(crate, Physics::FixedJoint{}) != nullptr);

    for (int32_t i = 0; i < kSettleSteps; ++i)
    {
        Step(test.world);
        CHECK(test.world.BrokenJoints().empty());
    }
    CHECK(test.scene.Has<Physics::FixedJoint>(crate));
}

TEST_CASE("A hinge that breaks takes its motor with it")
{
    TestScene test;
    const ECS::Entity crate = AddLevelArm(test.scene);
    Physics::HingeJoint hinge = LevelHinge(180.f);
    hinge.breakForce = 1.f;
    REQUIRE(test.scene.Add(crate, hinge) != nullptr);
    REQUIRE(test.scene.Add(crate, Physics::HingeMotor{}) != nullptr);

    Step(test.world, kSettleSteps / 4);

    CHECK_FALSE(test.scene.Has<Physics::HingeJoint>(crate));
    CHECK_FALSE(test.scene.Has<Physics::HingeMotor>(crate));
}

TEST_CASE("A world that does not break joints keeps them however hard they are pulled")
{
    TestScene test;
    test.world.SetBreaksJoints(false);
    const ECS::Entity crate = AddCrate(test.scene, kPivot);
    REQUIRE(test.scene.Add(crate, Physics::FixedJoint{.breakForce = 1.f}) != nullptr);

    Step(test.world, kSettleSteps / 4);

    CHECK(test.scene.Has<Physics::FixedJoint>(crate));
    CHECK(glm::length(PositionOf(test.scene, crate) - kPivot) < kHoldTolerance);
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

TEST_CASE("A joint waits for its other body, and goes when either body goes")
{
    TestScene test;
    const glm::vec3 postAt = kPivot + glm::vec3(2.f, 0.f, 0.f);
    const ECS::Entity post = test.scene.Create();
    REQUIRE(test.scene.Add(post, ECS::Transform{.position = postAt}) != nullptr);
    const ECS::Entity crate = AddCrate(test.scene, kPivot);
    REQUIRE(test.scene.Add(crate, Physics::FixedJoint{.other = post}) != nullptr);

    SUBCASE("no body yet: the crate falls")
    {
        Step(test.world, kSettleSteps / 4);
        CHECK(PositionOf(test.scene, crate).y < kPivot.y - 0.5f);
    }

    SUBCASE("the other body arrives: the crate is held")
    {
        Step(test.world);
        REQUIRE(test.scene.Add(post, Box(glm::vec3(kHalf), /*isStatic=*/ true).collider) != nullptr);
        Step(test.world);
        const glm::vec3 held = PositionOf(test.scene, crate);
        Step(test.world, kSettleSteps / 4);
        CHECK(glm::length(PositionOf(test.scene, crate) - held) < kHoldTolerance);
    }

    SUBCASE("the other body is destroyed: the crate falls")
    {
        REQUIRE(test.scene.Add(post, Box(glm::vec3(kHalf), /*isStatic=*/ true).collider) != nullptr);
        Step(test.world, kSettleSteps);
        REQUIRE(glm::length(PositionOf(test.scene, crate) - kPivot) < kHoldTolerance);
        // Held still, it has fallen asleep: letting go must wake it.
        REQUIRE_FALSE(test.world.IsBodyActive(crate));

        test.scene.Destroy(post);
        test.scene.FlushDestroyed();
        Step(test.world, kSettleSteps / 4);
        CHECK(PositionOf(test.scene, crate).y < kPivot.y - 0.5f);
    }

    SUBCASE("the other body loses its Collider and gets it back: the crate is held again")
    {
        REQUIRE(test.scene.Add(post, Box(glm::vec3(kHalf), /*isStatic=*/ true).collider) != nullptr);
        Step(test.world);
        (void)test.scene.Remove<Physics::Collider>(post);
        Step(test.world, kSettleSteps / 8);
        REQUIRE(PositionOf(test.scene, crate).y < kPivot.y - 0.1f);

        // The joint keeps the pose it was first built in, so the crate is
        // pulled back up to it rather than held where it fell to.
        REQUIRE(test.scene.Add(post, Box(glm::vec3(kHalf), /*isStatic=*/ true).collider) != nullptr);
        Step(test.world, kSettleSteps);
        CHECK(glm::length(PositionOf(test.scene, crate) - kPivot) < kHoldTolerance);
    }
}

TEST_CASE("Removing a joint from a body it held asleep lets the body fall")
{
    TestScene test;
    const ECS::Entity crate = AddCrate(test.scene, kPivot);
    REQUIRE(test.scene.Add(crate, Physics::FixedJoint{}) != nullptr);
    Step(test.world, kSettleSteps);
    REQUIRE_FALSE(test.world.IsBodyActive(crate));

    (void)test.scene.Remove<Physics::FixedJoint>(crate);
    Step(test.world, kSettleSteps / 4);

    CHECK(PositionOf(test.scene, crate).y < kPivot.y - 0.5f);
}

TEST_CASE("Destroying the body that carries a joint leaves the other body alone")
{
    TestScene test;
    (void)AddFloor(test.scene);
    const ECS::Entity resting = AddCrate(test.scene, glm::vec3(0.f, kHalf, 0.f));
    const ECS::Entity crate = AddCrate(test.scene, glm::vec3(0.f, kHalf, 2.f));
    REQUIRE(test.scene.Add(crate, Physics::PointJoint{.other = resting}) != nullptr);
    Step(test.world, kSettleSteps / 4);
    const glm::vec3 before = PositionOf(test.scene, resting);

    test.scene.Destroy(crate);
    test.scene.FlushDestroyed();
    REQUIRE_FALSE(test.scene.IsAlive(crate));
    Step(test.world, kSettleSteps / 4);

    CHECK(glm::length(PositionOf(test.scene, resting) - before) < kHoldTolerance);
}

TEST_CASE("A joint's other body rebuilt from static to moving keeps the joint's angles")
{
    TestScene test;
    const ECS::Entity post = AddBody(test.scene, kPivot, Box(glm::vec3(kHalf), /*isStatic=*/ true));
    const ECS::Entity crate = AddLevelArm(test.scene);
    Physics::HingeJoint hinge = LevelHinge(30.f);
    hinge.other = post;
    REQUIRE(test.scene.Add(crate, hinge) != nullptr);
    Step(test.world, kSettleSteps);
    REQUIRE(DropDegrees(test.scene, crate) == doctest::Approx(30.f).epsilon(0.1));

    // A RigidBody rebuilds the post's body. A joint built afresh from where
    // things are now would put its zero at 30 and let the arm drop to 60.
    Physics::RigidBody moving;
    moving.motion = Physics::MotionType::Kinematic;
    REQUIRE(test.scene.Add(post, moving) != nullptr);

    CHECK(MostDrop(test, crate, kSettleSteps) < 30.f + kAngleTolerance);
}

TEST_CASE("A chain follows its last link")
{
    Weightless test;
    constexpr int32_t kLinks = 4;
    constexpr float kSpacing = 1.f;
    ECS::Entity links[kLinks];
    for (int32_t i = 0; i < kLinks; ++i)
    {
        links[i] = AddCrate(test.scene, glm::vec3(static_cast<float>(i) * kSpacing, 0.f, 0.f));
        if (i > 0)
        {
            REQUIRE(test.scene.Add(links[i], Physics::PointJoint{.anchor = glm::vec3(-kSpacing * 0.5f, 0.f, 0.f),
                                                                 .other = links[i - 1]}) != nullptr);
        }
    }
    SetVelocity(test.scene, links[kLinks - 1], glm::vec3(5.f, 0.f, 0.f), glm::vec3(0.f));

    Step(test.world, kSettleSteps / 2);

    CHECK(PositionOf(test.scene, links[0]).x > 1.f);
}

TEST_CASE("A long chain swinging a heavy body barely stretches")
{
    TestScene test;
    constexpr int32_t kLinks = 16;
    constexpr float kGap = 5.f;
    constexpr float kLinkRadius = 0.15f;
    constexpr float kSteel = 7800.f;
    // Four seconds, two full swings down and back.
    constexpr int32_t kSwingSteps = kSettleSteps * 2;
    // How much longer than built the chain may pull out: a few percent at the
    // solver passes joints get, half again its length at the world's default.
    constexpr float kMostStretch = 1.05f;
    constexpr float kSpacing = kGap / static_cast<float>(kLinks);
    constexpr glm::vec3 kAlong{1.f, 0.f, 0.f};

    // The far cube starts level with the fixed one, so it swings down on the
    // chain and pulls hardest at the bottom.
    BodySpec cube = Box(glm::vec3(0.5f), /*isStatic=*/ true);
    cube.collider.density = kSteel;
    const ECS::Entity fixed = AddBody(test.scene, kPivot, cube);
    const glm::vec3 face = kPivot + kAlong * 0.5f;
    ECS::Entity previous = fixed;
    for (int32_t i = 0; i < kLinks; ++i)
    {
        BodySpec ball = Ball(kLinkRadius, /*isStatic=*/ false);
        ball.collider.density = kSteel;
        const ECS::Entity link = AddBody(test.scene, face + kAlong * (kSpacing * (static_cast<float>(i) + 0.5f)), ball);
        REQUIRE(test.scene.Add(link, Physics::PointJoint{.anchor = -kAlong * (kSpacing * 0.5f), .other = previous}) !=
                nullptr);
        previous = link;
    }
    cube.rigidBody = Physics::RigidBody{};
    const ECS::Entity swinging = AddBody(test.scene, face + kAlong * (kGap + 0.5f), cube);
    REQUIRE(test.scene.Add(swinging, Physics::PointJoint{.anchor = -kAlong * 0.5f, .other = previous}) != nullptr);

    const float built = glm::length(PositionOf(test.scene, swinging) - kPivot);
    float longest = built;
    for (int32_t i = 0; i < kSwingSteps; ++i)
    {
        Step(test.world);
        longest = std::max(longest, glm::length(PositionOf(test.scene, swinging) - kPivot));
    }

    CHECK(PositionOf(test.scene, swinging).y < kPivot.y - kGap * 0.5f);
    CHECK(longest < built * kMostStretch);
}

// ---------------------------------------------------------------------------
// Self-collision
// ---------------------------------------------------------------------------

TEST_CASE("Bodies joined directly do not collide unless collideConnected says so, and still collide with others")
{
    int32_t withThird = 0;
    CHECK(ContactsWhenJoined(false, withThird) == 0);
    CHECK(withThird == 2);
    CHECK(ContactsWhenJoined(true, withThird) == 1);
    CHECK(withThird == 2);
}
