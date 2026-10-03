/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PhysicsTestScene.hpp
/// @brief A scene with a physics world bound to it, and the few ways the tests
///        put bodies in it.
///
/// A body exists because an entity has a descriptor and a Transform, so every
/// helper here builds an entity; none of them talks to the world directly. The
/// world builds the body on its next reconcile, which Step runs.

#include <cstdint>

#include <doctest/doctest.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

namespace Assisi::PhysicsTests
{

/// The fixed step every test simulates at.
inline constexpr float kStep = 1.f / 60.f;

/// A scene and its world. Pinned, as both are: tests construct one in place.
struct TestScene
{
    explicit TestScene(uint32_t maxBodies = Physics::kDefaultMaxBodies) : world(scene, maxBodies) {}

    ECS::Scene scene;
    Physics::PhysicsWorld world;
};

/// A box descriptor of @p halfExtents.
inline Physics::RigidBodyDescriptor Box(glm::vec3 halfExtents, bool isStatic)
{
    Physics::RigidBodyDescriptor descriptor;
    descriptor.shape = Physics::ColliderShape::Box;
    descriptor.halfExtents = halfExtents;
    descriptor.isStatic = isStatic;
    return descriptor;
}

/// A sphere descriptor of @p radius.
inline Physics::RigidBodyDescriptor Ball(float radius, bool isStatic)
{
    Physics::RigidBodyDescriptor descriptor;
    descriptor.shape = Physics::ColliderShape::Sphere;
    descriptor.radius = radius;
    descriptor.isStatic = isStatic;
    return descriptor;
}

/// An entity at @p position carrying @p descriptor.
inline ECS::Entity AddBody(ECS::Scene &scene, glm::vec3 position, const Physics::RigidBodyDescriptor &descriptor)
{
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, ECS::Transform{.position = position}) != nullptr);
    REQUIRE(scene.Add(entity, descriptor) != nullptr);
    return entity;
}

/// A character standing with its feet at @p feet.
inline ECS::Entity AddCharacter(ECS::Scene &scene, glm::vec3 feet,
                                const Physics::CharacterDescriptor &descriptor = Physics::CharacterDescriptor{})
{
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, ECS::Transform{.position = feet}) != nullptr);
    REQUIRE(scene.Add(entity, descriptor) != nullptr);
    return entity;
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
