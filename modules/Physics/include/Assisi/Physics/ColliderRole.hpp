/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ColliderRole.hpp
/// @brief What a Collider is to the simulation, decided by what is above it.

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/Scene.hpp>

#include <cstdint>

namespace Assisi::Physics
{

/// @brief What a Collider is to the simulation.
///
/// Decided by walking up the entity's Parent chain to the first RigidBody or
/// Character. RigidBody and Character exclude Parent, so that one is always the
/// root of the chain.
enum class ColliderRole : std::uint8_t
{
    /// On the RigidBody's own entity: the body's own shape.
    Own,

    /// Nothing above it moves: its own static body.
    Static,

    /// Under a RigidBody: a rigid part of that body's shape, adding to its
    /// mass.
    Piece,

    /// Under a Character, on the Trigger channel under a RigidBody, or set to
    /// ColliderAttach::Body: its own kinematic body, put at its entity's pose
    /// after every step.
    Follower,
    Count_,
};

/// @brief A Collider's role, and the entity whose body it answers for.
struct ColliderPlacement
{
    /// The RigidBody or Character entity a Piece or a Follower belongs to; the
    /// collider's own entity for Own and Static.
    ECS::Entity owner{ECS::NullEntity};

    ColliderRole role = ColliderRole::Static;
};

/// @brief What @p entity's Collider is, read from the scene alone.
///
/// Answers for an entity with or without a Collider, enabled or not, so the
/// editor can say what a collider would be before anything has stepped. A
/// Parent naming an entity with no Transform ends the chain there, as it ends
/// the composed pose.
[[nodiscard]] ColliderPlacement ResolveColliderPlacement(const ECS::Scene &scene, ECS::Entity entity);

} // namespace Assisi::Physics
