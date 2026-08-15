/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBodyLifetime.cpp
/// @brief What RemoveBody does with a handle that never named a body.
///
/// `Physics::RigidBody` is an aggregate holding a bare `JPH::BodyID`, so a
/// default-constructed one is not a null pointer that crashes loudly — it is
/// `cInvalidBodyID`, and handing it to Jolt's body interface is undefined rather
/// than refused. RemoveBody's `IsInvalid()` early-return is what stands between
/// the two, and every caller that keeps a handle in a container leans on it: a
/// map's `operator[]` value-initializes on a miss, so a lookup that was meant to
/// find an existing record can produce a blank one instead, and the blank one is
/// removed alongside the real ones at teardown.
///
/// `ReplicationClientMotion.cpp`'s `_bodies[state.netId]` is that reading, and it
/// is unreachable — but only by an invariant spanning NetSync and the editor's
/// inspector that rests partly on a disabled ImGui region rather than a check.
/// This file pins the other half: harmless even if it were reached, which is one
/// guard in one function and survives a refactor of the first half.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

using namespace Assisi;

namespace
{

constexpr float kStep = 1.f / 60.f;

constexpr Physics::PhysicsWorld::ColliderShapeDesc kBall{.shape = Physics::ColliderShape::Sphere, .radius = 0.5f};

} // namespace

TEST_CASE("RemoveBody refuses a default-constructed handle instead of passing it to Jolt")
{
    Physics::PhysicsWorld world;

    // A default-constructed handle names nothing: this is what a map's
    // `operator[]` produces on a miss, and what a caller iterating its records at
    // teardown would hand over without ever looking at it.
    const Physics::RigidBody unnamed;
    REQUIRE(unnamed.bodyId.IsInvalid());

    world.RemoveBody(unnamed);
    // Twice, because a guard that only worked by luck the first time — say one
    // that erased bookkeeping before checking — would not survive being asked
    // again with nothing left to erase.
    world.RemoveBody(unnamed);
}

TEST_CASE("GetBodyTransform refuses a handle that names nothing, like its sibling accessors")
{
    // ENG-117. PhysicsWorld.cpp:914 is the one accessor with no `IsAdded` guard —
    // GetBodyVelocity (:930) and IsBodyCCDEnabled (:944) both check first, and
    // RemoveBody checks `IsInvalid()`. This pins what the unguarded path actually
    // does with the two handles a caller can hold by mistake, so the guard can be
    // added without guessing at the behaviour it has to preserve.
    Physics::PhysicsWorld world;

    const Physics::RigidBody unnamed;
    REQUIRE(unnamed.bodyId.IsInvalid());

    const auto [position, rotation] = world.GetBodyTransform(unnamed);
    CHECK(position == glm::vec3(0.f));
    CHECK(rotation.w == doctest::Approx(1.f)); // identity, not garbage

    // The other reachable shape: a handle that named a real body until it was
    // removed. The velocity accessor answers zero for this one by contract.
    const Physics::RigidBody removed =
        world.AddBody({3.f, 4.f, 5.f}, glm::quat{1.f, 0.f, 0.f, 0.f}, kBall, Physics::BodyMotion::Dynamic);
    REQUIRE_FALSE(removed.bodyId.IsInvalid());
    world.RemoveBody(removed);

    const auto [staleLinear, staleAngular] = world.GetBodyVelocity(removed);
    CHECK(staleLinear == glm::vec3(0.f));
    CHECK(staleAngular == glm::vec3(0.f));

    const auto [stalePosition, staleRotation] = world.GetBodyTransform(removed);
    CHECK(stalePosition == glm::vec3(0.f));
    CHECK(staleRotation.w == doctest::Approx(1.f));
}

TEST_CASE("removing a handle that names nothing leaves the bodies that do alone")
{
    // The half that matters in practice. `Reset()` walks a container of records
    // and removes every one; a blank record among them must cost the walk
    // nothing, rather than taking a real body down with it or corrupting the
    // world's own bookkeeping on the way past.
    Physics::PhysicsWorld world;

    const Physics::RigidBody ground =
        world.AddBody({0.f, -1.f, 0.f}, glm::quat{1.f, 0.f, 0.f, 0.f},
                      Physics::PhysicsWorld::ColliderShapeDesc{.shape       = Physics::ColliderShape::Box,
                                                               .halfExtents = {20.f, 1.f, 20.f}},
                      Physics::BodyMotion::Static);
    const Physics::RigidBody falling =
        world.AddBody({0.f, 4.f, 0.f}, glm::quat{1.f, 0.f, 0.f, 0.f}, kBall, Physics::BodyMotion::Dynamic);
    REQUIRE_FALSE(ground.bodyId.IsInvalid());
    REQUIRE_FALSE(falling.bodyId.IsInvalid());

    world.Update(kStep);
    world.CaptureState();
    const auto [beforePosition, beforeRotation] = world.GetBodyTransform(falling);
    (void)beforeRotation;
    REQUIRE(world.IsBodyActive(falling));

    world.RemoveBody(Physics::RigidBody{});

    // Both real bodies are still there, and still the ones they were: the dynamic
    // one keeps falling under gravity and the static one keeps catching it.
    CHECK(world.IsBodyActive(falling));
    for (int32_t step = 0; step < 240; ++step)
    {
        world.Update(kStep);
        world.CaptureState();
    }

    const auto [afterPosition, afterRotation] = world.GetBodyTransform(falling);
    (void)afterRotation;
    CHECK(afterPosition.y < beforePosition.y);        // it fell
    CHECK(afterPosition.y > 0.f);                     // ...and the floor stopped it
    CHECK(world.GetBodyTransform(ground).second.w > 0.9f); // the static body is untouched

    // And the active-set bookkeeping the removal walks is intact: a settled world
    // reports nothing active, which it cannot do from a corrupted body list.
    std::vector<Physics::PhysicsWorld::ActiveBodyState> active;
    world.GetActiveBodyStates(active);
    CHECK(active.empty());
}
