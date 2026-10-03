/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestParentedBodies.cpp
/// @brief PhysicsWorld's two conversions for a parented entity — local pose in on
/// creation, world pose out on writeback.
///
/// Jolt places and reports bodies in world space; a Transform under a parent is an
/// offset *from* that parent. Physics reads the parent's propagated world matrix
/// through ECS::ParentWorldMatrix. Get either half wrong and the failure is
/// silent: the body starts at its local pose, and every frame the writeback stores
/// a world pose that transform propagation then multiplies by the parent again — a
/// body that drifts by its parent's transform, forever.
///
/// Blueprint members are routinely both parented and physics-driven — a car's
/// wheels are exactly that.

#include <doctest/doctest.h>

#include <cstdint>

#include <Assisi/ECS/Hierarchy.hpp>
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

/// Loose enough for a quaternion round-tripped through two matrix casts, tight
/// enough that applying a 90° parent rotation one time too many cannot pass.
constexpr float kEpsilon = 1e-4f;

/// A parent pose that is neither identity nor axis-aligned with the child's, so
/// a missing conversion cannot coincidentally produce the right answer.
const ECS::Transform kParentPose{.position = {10.f, 2.f, -3.f},
                                 .rotation = glm::angleAxis(glm::radians(90.f), glm::vec3(0.f, 1.f, 0.f))};

bool NearlyEqual(glm::vec3 a, glm::vec3 b, float epsilon = kEpsilon)
{
    return glm::all(glm::lessThan(glm::abs(a - b), glm::vec3(epsilon)));
}

/// |dot| folds the q/-q double cover: two quaternions naming the same orientation
/// may differ in sign.
bool NearlyEqual(glm::quat a, glm::quat b, float epsilon = kEpsilon)
{
    return glm::abs(1.f - glm::abs(glm::dot(a, b))) < epsilon;
}

constexpr Physics::RigidBodyDescriptor kBall{.shape = Physics::ColliderShape::Sphere, .radius = 0.5f};

/// @p child placed under a new entity posed at kParentPose, with world matrices
/// propagated so the parent's is current. Returns the parent's world matrix.
glm::mat4 ParentUnder(ECS::Scene &scene, ECS::Entity child)
{
    const ECS::Entity parent = scene.Create();
    REQUIRE(scene.Add(parent, kParentPose) != nullptr);
    REQUIRE(scene.Add(child, ECS::Parent{.parent = parent}) != nullptr);
    (void)ECS::PropagateTransforms(scene, 0);
    return *ECS::WorldMatrix(scene, parent);
}

/// Gives @p entity a ball body, built by the next reconcile.
void AddBall(ECS::Scene &scene, Physics::PhysicsWorld &world, ECS::Entity entity)
{
    REQUIRE(scene.Add(entity, kBall) != nullptr);
    world.Reconcile();
}

} // namespace

TEST_CASE("A parented body is created at its composed world pose")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene};

    const ECS::Transform local{.position = {1.f, 0.f, 0.f},
                               .rotation = glm::angleAxis(glm::radians(30.f), glm::vec3(0.f, 1.f, 0.f))};

    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, local) != nullptr);
    const glm::mat4 parent = ParentUnder(scene, entity);

    AddBall(scene, world, entity);

    const Physics::Pose pose = world.GetBodyPose(entity);
    CHECK(NearlyEqual(pose.position, glm::vec3(parent * glm::vec4(local.position, 1.f))));
    CHECK(NearlyEqual(pose.rotation, glm::quat_cast(glm::mat3(parent)) * local.rotation));
}

TEST_CASE("An entity without a Parent takes its local pose as world")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene};

    const ECS::Transform local{.position = {1.f, 0.f, 0.f}};

    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, local) != nullptr);

    AddBall(scene, world, entity);
    CHECK(NearlyEqual(world.GetBodyPose(entity).position, local.position));
}

TEST_CASE("A parent with no Transform defines no space, so the local pose is world")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene};

    const ECS::Transform local{.position = {1.f, 0.f, 0.f}};

    const ECS::Entity poseless = scene.Create();
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, local) != nullptr);
    REQUIRE(scene.Add(entity, ECS::Parent{.parent = poseless}) != nullptr);
    (void)ECS::PropagateTransforms(scene, 0);

    AddBall(scene, world, entity);
    CHECK(NearlyEqual(world.GetBodyPose(entity).position, local.position));
}

TEST_CASE("InterpolateTransforms: a parented body's world pose decomposes back to the local field")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene};

    const ECS::Transform local{.position = {1.f, 0.f, 0.f},
                               .rotation = glm::angleAxis(glm::radians(30.f), glm::vec3(0.f, 1.f, 0.f))};

    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, local) != nullptr);
    const glm::mat4 parent = ParentUnder(scene, entity);
    AddBall(scene, world, entity);

    // Two steps so both interpolation snapshots straddle a real displacement; the
    // ball falls under gravity, so the world pose is now something the parent
    // frame definitely does not equal.
    for (int32_t i = 0; i < 2; ++i)
    {
        world.Update(kStep);
        world.CaptureState();
    }

    world.InterpolateTransforms(1.f);

    const ECS::Transform *written = scene.Get<ECS::Transform>(entity);
    REQUIRE(written != nullptr);

    // Recomposed rather than re-derived: asserting that parent × local equals the
    // body's world pose tests the round trip without restating the same algebra
    // the writeback used, which would pass even if both halves were wrong.
    const Physics::Pose worldPose = world.GetBodyPose(entity);
    CHECK(NearlyEqual(glm::vec3(parent * glm::vec4(written->position, 1.f)), worldPose.position));
    CHECK(NearlyEqual(glm::quat_cast(glm::mat3(parent)) * written->rotation, worldPose.rotation));

    // And it actually fell: a writeback that silently did nothing would satisfy the
    // round trip above with the spawn pose still in place.
    CHECK(written->position.y < local.position.y - 1e-3f);
}

TEST_CASE("InterpolateTransforms: an unparented body in a parented scene is untouched by the conversion")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene};

    // One parented entity beside a loose one — the ordinary case in any scene
    // holding one instance and a hundred loose entities. The loose one must come
    // through unchanged.
    const ECS::Entity parented = scene.Create();
    REQUIRE(scene.Add(parented, ECS::Transform{}) != nullptr);
    (void)ParentUnder(scene, parented);

    const ECS::Transform local{.position = {4.f, 8.f, 0.f}};
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, local) != nullptr);

    AddBall(scene, world, entity);
    CHECK(NearlyEqual(world.GetBodyPose(entity).position, local.position));

    for (int32_t i = 0; i < 2; ++i)
    {
        world.Update(kStep);
        world.CaptureState();
    }
    world.InterpolateTransforms(1.f);

    const ECS::Transform *written = scene.Get<ECS::Transform>(entity);
    REQUIRE(written != nullptr);
    CHECK(NearlyEqual(written->position, world.GetBodyPose(entity).position));
}
