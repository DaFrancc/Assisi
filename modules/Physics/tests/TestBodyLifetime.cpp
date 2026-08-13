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
/// That is exactly the reading of `ReplicationClient::ApplyBodyState` that ENG-122
/// reported. The branch turned out to be unreachable — see the comment at
/// ReplicationClientMotion.cpp's `_bodies[state.netId]` for why — but the argument
/// closing it has two legs, and this is the second: unreachable *and* harmless if
/// it were ever reached. The first leg is an invariant spanning NetSync and the
/// editor's inspector, and it rests in part on a disabled ImGui region rather than
/// a check — a refactor could break it without noticing. This one is a single
/// guard in a single function, so pinning it here is what keeps the second leg
/// true independently of the first.

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
