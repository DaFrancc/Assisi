/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Physics/ColliderRole.hpp>

#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

#include <cstdint>

namespace Assisi::Physics
{

namespace
{

/// The most Parent links followed. A cycle is a corrupt scene, and the answer
/// to one is a wrong role, not a hang; the same bound the composed pose uses.
constexpr uint32_t kMaxDepth = 256;

/// Whether a collider under a RigidBody rides along rather than being part of
/// it. Jolt makes a whole body a sensor or none of it, so a trigger cannot be a
/// piece of a solid body.
bool RidesAlong(const Collider *collider)
{
    return collider != nullptr &&
           (collider->attach == ColliderAttach::Body || collider->channel == CollisionChannel::Trigger);
}

} // namespace

ColliderPlacement ResolveColliderPlacement(const ECS::Scene &scene, ECS::Entity entity)
{
    if (scene.Has<RigidBody>(entity))
    {
        return ColliderPlacement{entity, ColliderRole::Own};
    }

    ECS::Entity current = entity;
    for (uint32_t depth = 0; depth < kMaxDepth; ++depth)
    {
        const ECS::Parent *parent = scene.Get<ECS::Parent>(current);
        if (parent == nullptr || parent->parent == ECS::NullEntity || !scene.Has<ECS::Transform>(parent->parent))
        {
            break;
        }
        current = parent->parent;

        if (scene.Has<Character>(current))
        {
            return ColliderPlacement{current, ColliderRole::Follower};
        }
        if (scene.Has<RigidBody>(current))
        {
            const ColliderRole role = RidesAlong(scene.Get<Collider>(entity)) ? ColliderRole::Follower
                                                                               : ColliderRole::Piece;
            return ColliderPlacement{current, role};
        }
    }
    return ColliderPlacement{entity, ColliderRole::Static};
}

} // namespace Assisi::Physics
