/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PhysicsTestScene.hpp
/// @brief A scene with a physics world bound to it, and the few ways the tests
///        put bodies in it.
///
/// A body exists because an entity has a Collider or a Character and a
/// Transform, so every helper here builds an entity or writes a component; none
/// of them talks to the world directly. The world follows on its next
/// reconcile, which Step runs.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>

#include <doctest/doctest.h>

#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

namespace Assisi::PhysicsTests
{

/// The fixed step every test simulates at.
inline constexpr float kStep = 1.f / 60.f;

/// Collision models held in memory, for a test to name by id.
class TestCollisionSource final : public Physics::CollisionSource
{
public:
    void Add(Core::AssetId id, Geometry::CollisionModel model) { _models[id] = std::move(model); }

    std::optional<Geometry::CollisionModel> Load(Core::AssetId id) const override
    {
        ++loads;
        const std::unordered_map<Core::AssetId, Geometry::CollisionModel>::const_iterator found = _models.find(id);
        if (found == _models.end())
        {
            return std::nullopt;
        }
        return found->second;
    }

    /// How many times a world has read a model.
    mutable std::atomic<uint32_t> loads = 0;

private:
    std::unordered_map<Core::AssetId, Geometry::CollisionModel> _models;
};

/// A scene and its world. Pinned, as both are: tests construct one in place.
struct TestScene
{
    explicit TestScene(uint32_t maxBodies = Physics::kDefaultMaxBodies) : world(scene, collision, maxBodies) {}

    ECS::Scene scene;

    /// Declared before the world, which reads models from it.
    TestCollisionSource collision;
    Physics::PhysicsWorld world;
};

/// A collider, and the RigidBody that makes it move unless it is static.
struct BodySpec
{
    Physics::Collider collider;
    std::optional<Physics::RigidBody> rigidBody;
};

/// A box of @p halfExtents, static or dynamic.
inline BodySpec Box(glm::vec3 halfExtents, bool isStatic)
{
    BodySpec spec;
    spec.collider.shape = Physics::ColliderShape::Box;
    spec.collider.halfExtents = halfExtents;
    if (!isStatic)
    {
        spec.rigidBody = Physics::RigidBody{};
    }
    return spec;
}

/// A sphere of @p radius, static or dynamic.
inline BodySpec Ball(float radius, bool isStatic)
{
    BodySpec spec;
    spec.collider.shape = Physics::ColliderShape::Sphere;
    spec.collider.radius = radius;
    if (!isStatic)
    {
        spec.rigidBody = Physics::RigidBody{};
    }
    return spec;
}

/// @p spec on @p filter. A moving body on the Trigger channel becomes a
/// kinematic sensor that never sleeps, the kind that finds bodies already at
/// rest inside it.
inline BodySpec WithFilter(BodySpec spec, Physics::CollisionFilter filter)
{
    spec.collider.channel = filter.channel;
    spec.collider.collidesWith = filter.collidesWith;
    if (spec.rigidBody.has_value() && filter.channel == Physics::CollisionChannel::Trigger)
    {
        spec.rigidBody->motion = Physics::MotionType::Kinematic;
        spec.rigidBody->allowSleep = false;
    }
    return spec;
}

/// An entity at @p position carrying @p spec.
inline ECS::Entity AddBody(ECS::Scene &scene, glm::vec3 position, const BodySpec &spec)
{
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, ECS::Transform{.position = position}) != nullptr);
    REQUIRE(scene.Add(entity, spec.collider) != nullptr);
    if (spec.rigidBody.has_value())
    {
        REQUIRE(scene.Add(entity, *spec.rigidBody) != nullptr);
    }
    return entity;
}

/// An entity under @p parent, at @p localPosition in the parent's space,
/// carrying @p collider.
inline ECS::Entity AddChildCollider(ECS::Scene &scene, ECS::Entity parent, glm::vec3 localPosition,
                                    const Physics::Collider &collider)
{
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, ECS::Transform{.position = localPosition}) != nullptr);
    REQUIRE(scene.Add(entity, ECS::Parent{.parent = parent}) != nullptr);
    REQUIRE(scene.Add(entity, collider) != nullptr);
    return entity;
}

/// A character standing with its feet at @p feet.
inline ECS::Entity AddCharacter(ECS::Scene &scene, glm::vec3 feet,
                                const Physics::Character &character = Physics::Character{})
{
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, ECS::Transform{.position = feet}) != nullptr);
    REQUIRE(scene.Add(entity, character) != nullptr);
    return entity;
}

/// What @p character's last step left behind.
inline Physics::CharacterState StateOf(const ECS::Scene &scene, ECS::Entity character)
{
    const Physics::CharacterState *state = scene.Get<Physics::CharacterState>(character);
    REQUIRE(state != nullptr);
    return *state;
}

/// Asks @p character to move at @p wishVelocity, in metres per second, and,
/// with @p jump, to jump. The intent's `move` is scaled by the character's
/// speed for the stance it is in, so it is divided by that here.
inline void Drive(ECS::Scene &scene, ECS::Entity character, glm::vec3 wishVelocity, bool jump)
{
    const Physics::Character &tuning = *scene.Get<Physics::Character>(character);
    const bool crouching = StateOf(scene, character).stance == Physics::Stance::Crouching;
    const float speed = crouching ? tuning.walkSpeed * tuning.crouchSpeedScale : tuning.walkSpeed;
    Physics::CharacterIntent &intent = *scene.GetMut<Physics::CharacterIntent>(character);
    intent.move = speed > 0.f ? wishVelocity / speed : glm::vec3(0.f);
    intent.jump = intent.jump || jump;
}

/// Asks @p character for @p stance and applies it now, through a reconcile.
/// @return whether the character is in that stance afterwards.
inline bool AskStance(ECS::Scene &scene, Physics::PhysicsWorld &world, ECS::Entity character, Physics::Stance stance)
{
    scene.GetMut<Physics::CharacterIntent>(character)->stance = stance;
    world.Reconcile();
    return StateOf(scene, character).stance == stance;
}

/// Turns @p character to look along @p forward, ignoring its vertical part.
inline void Face(ECS::Scene &scene, ECS::Entity character, glm::vec3 forward)
{
    const glm::vec3 flat = glm::normalize(glm::vec3(forward.x, 0.f, forward.z));
    const float yaw = std::atan2(-flat.x, -flat.z);
    scene.GetMut<ECS::Transform>(character)->rotation = glm::angleAxis(yaw, glm::vec3(0.f, 1.f, 0.f));
}

/// A wide static floor whose top face is at height 0.
inline ECS::Entity AddFloor(ECS::Scene &scene)
{
    constexpr float kHalfWidth = 50.f;
    constexpr float kHalfThickness = 0.5f;
    return AddBody(scene, {0.f, -kHalfThickness, 0.f}, Box({kHalfWidth, kHalfThickness, kHalfWidth}, true));
}

/// Runs @p steps fixed steps.
inline void Step(Physics::PhysicsWorld &world, int32_t steps = 1)
{
    for (int32_t i = 0; i < steps; ++i)
    {
        world.Update(kStep);
    }
}

} // namespace Assisi::PhysicsTests
