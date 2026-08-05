/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestParentedBodies.cpp
/// @brief PhysicsWorld's two conversions for a parented entity — local pose in on
/// creation, world pose out on writeback.
///
/// Jolt places and reports bodies in world space; a Transform under a parent is an
/// offset *from* that parent. Nothing in Physics can see the parent link (it lives
/// in Runtime, which Physics deliberately does not link), so the world matrix is
/// handed down as a PhysicsWorld::ParentWorldFn. Get either half wrong and the
/// failure is silent: the body starts at its local pose, and every frame the
/// writeback stores a world pose that transform propagation then multiplies by the
/// parent again — a body that drifts by its parent's transform, forever.
///
/// Nothing was ever both parented and physics-driven before blueprints, which is
/// why this went unnoticed; a car's wheels are exactly that
/// (docs/blueprint-system-concept.md §12).
///
/// The tests supply their own resolver rather than a Runtime::Parent chain, which
/// is the honest scope: the contract PhysicsWorld has is with the callable, and
/// keeping it that way is what lets this suite link Physics alone.

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

/// Loose enough for a quaternion round-tripped through two matrix casts, tight
/// enough that applying a 90° parent rotation one time too many cannot pass.
constexpr float kEpsilon = 1e-4f;

/// A parent frame that is neither identity nor axis-aligned with the child's, so
/// a missing conversion cannot coincidentally produce the right answer.
glm::mat4 ParentMatrix()
{
    return glm::translate(glm::mat4(1.f), glm::vec3(10.f, 2.f, -3.f)) *
           glm::rotate(glm::mat4(1.f), glm::radians(90.f), glm::vec3(0.f, 1.f, 0.f));
}

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

} // namespace

TEST_CASE("AddBodyFromDescriptor: a parented body is created at its composed world pose")
{
    ECS::Scene            scene;
    Physics::PhysicsWorld world;

    const glm::mat4      parent = ParentMatrix();
    const ECS::Transform local{.position = {1.f, 0.f, 0.f},
                               .rotation = glm::angleAxis(glm::radians(30.f), glm::vec3(0.f, 1.f, 0.f))};

    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, local) != nullptr);

    const Physics::RigidBody body = world.AddBodyFromDescriptor(
        scene, entity, local, kBall, [&parent](ECS::Entity) -> const glm::mat4 * { return &parent; });

    const auto [position, rotation] = world.GetBodyTransform(body);
    CHECK(NearlyEqual(position, glm::vec3(parent * glm::vec4(local.position, 1.f))));
    CHECK(NearlyEqual(rotation, glm::quat_cast(glm::mat3(parent)) * local.rotation));
}

TEST_CASE("AddBodyFromDescriptor: without a resolver the local pose is taken as world")
{
    ECS::Scene            scene;
    Physics::PhysicsWorld world;

    // The bug this whole file is about, pinned as behaviour: with nothing to ask,
    // Physics can only read the Transform as world space. That is correct for the
    // unparented entity it assumes, and is why the resolver has to be passed at
    // every site that might see a parented one.
    const ECS::Transform local{.position = {1.f, 0.f, 0.f}};

    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, local) != nullptr);

    const Physics::RigidBody body     = world.AddBodyFromDescriptor(scene, entity, local, kBall);
    const auto [position, rotation]   = world.GetBodyTransform(body);
    CHECK(NearlyEqual(position, local.position));
}

TEST_CASE("InterpolateTransforms: a parented body's world pose decomposes back to the local field")
{
    ECS::Scene            scene;
    Physics::PhysicsWorld world;

    const glm::mat4      parent = ParentMatrix();
    const auto           resolve = [&parent](ECS::Entity) -> const glm::mat4 * { return &parent; };
    const ECS::Transform local{.position = {1.f, 0.f, 0.f},
                               .rotation = glm::angleAxis(glm::radians(30.f), glm::vec3(0.f, 1.f, 0.f))};

    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, local) != nullptr);
    const Physics::RigidBody body = world.AddBodyFromDescriptor(scene, entity, local, kBall, resolve);

    // Two steps so both interpolation snapshots straddle a real displacement; the
    // ball falls under gravity, so the world pose is now something the parent
    // frame definitely does not equal.
    for (int32_t i = 0; i < 2; ++i)
    {
        world.Update(kStep);
        world.CaptureState();
    }

    world.InterpolateTransforms(scene, 1.f, resolve);

    const ECS::Transform *written = scene.Get<ECS::Transform>(entity);
    REQUIRE(written != nullptr);

    // Recomposed rather than re-derived: asserting that parent × local equals the
    // body's world pose tests the round trip without restating the same algebra
    // the writeback used, which would pass even if both halves were wrong.
    const auto [worldPosition, worldRotation] = world.GetBodyTransform(body);
    CHECK(NearlyEqual(glm::vec3(parent * glm::vec4(written->position, 1.f)), worldPosition));
    CHECK(NearlyEqual(glm::quat_cast(glm::mat3(parent)) * written->rotation, worldRotation));

    // And it actually fell: a writeback that silently did nothing would satisfy the
    // round trip above with the spawn pose still in place.
    CHECK(written->position.y < local.position.y - 1e-3f);
}

TEST_CASE("InterpolateTransforms: an unparented body in a parented scene is untouched by the conversion")
{
    ECS::Scene            scene;
    Physics::PhysicsWorld world;

    // A live resolver that answers null for *this* entity — the ordinary case in
    // any scene holding one instance and a hundred loose entities. It must cost
    // nothing and change nothing.
    const glm::mat4      parent  = ParentMatrix();
    const ECS::Entity    parented = scene.Create();
    const ECS::Transform local{.position = {4.f, 8.f, 0.f}};
    const ECS::Entity    entity = scene.Create();
    REQUIRE(scene.Add(entity, local) != nullptr);

    const auto resolve = [&parent, parented](ECS::Entity e) -> const glm::mat4 *
    { return e == parented ? &parent : nullptr; };

    const Physics::RigidBody body = world.AddBodyFromDescriptor(scene, entity, local, kBall, resolve);
    CHECK(NearlyEqual(world.GetBodyTransform(body).first, local.position));

    for (int32_t i = 0; i < 2; ++i)
    {
        world.Update(kStep);
        world.CaptureState();
    }
    world.InterpolateTransforms(scene, 1.f, resolve);

    const ECS::Transform *written = scene.Get<ECS::Transform>(entity);
    REQUIRE(written != nullptr);
    CHECK(NearlyEqual(written->position, world.GetBodyTransform(body).first));
}
